/*
 * Builtin "git pull"
 *
 * Based on git-pull.sh by Junio C Hamano
 *
 * Fetch one or more remote refs and merge it/them into the current HEAD.
 */
#include "cache.h"
#include "builtin.h"
#include "parse-options.h"
#include "exec_cmd.h"
#include "run-command.h"
#include "sha1-array.h"
#include "remote.h"
#include "dir.h"
#include "refs.h"
#include "revision.h"
#include "tempfile.h"
#include "lockfile.h"
#include "wt-status.h"

enum rebase_type {
	REBASE_INVALID = -1,
	REBASE_FALSE = 0,
	REBASE_TRUE,
	REBASE_PRESERVE,
	REBASE_INTERACTIVE
};

/**
 * Parses the value of --rebase. If value is a false value, returns
 * REBASE_FALSE. If value is a true value, returns REBASE_TRUE. If value is
 * "preserve", returns REBASE_PRESERVE. If value is a invalid value, dies with
 * a fatal error if fatal is true, otherwise returns REBASE_INVALID.
 */
static enum rebase_type parse_config_rebase(const char *key, const char *value,
		int fatal)
{
	int v = git_config_maybe_bool("pull.rebase", value);

	if (!v)
		return REBASE_FALSE;
	else if (v > 0)
		return REBASE_TRUE;
	else if (!strcmp(value, "preserve"))
		return REBASE_PRESERVE;
	else if (!strcmp(value, "interactive"))
		return REBASE_INTERACTIVE;

	if (fatal)
		die(_("Invalid value for %s: %s"), key, value);
	else
		error(_("Invalid value for %s: %s"), key, value);

	return REBASE_INVALID;
}

/**
 * Callback for --rebase, which parses arg with parse_config_rebase().
 */
static int parse_opt_rebase(const struct option *opt, const char *arg, int unset)
{
	enum rebase_type *value = opt->value;

	if (arg)
		*value = parse_config_rebase("--rebase", arg, 0);
	else
		*value = unset ? REBASE_FALSE : REBASE_TRUE;
	return *value == REBASE_INVALID ? -1 : 0;
}

static const char * const pull_usage[] = {
	N_("git pull [<options>] [<repository> [<refspec>...]]"),
	NULL
};

/* Shared options */
static int opt_verbosity;
static char *opt_progress;

/* Options passed to git-merge or git-rebase */
static enum rebase_type opt_rebase = -1;
static char *opt_diffstat;
static char *opt_log;
static char *opt_squash;
static char *opt_commit;
static char *opt_edit;
static char *opt_ff;
static char *opt_verify_signatures;
static struct argv_array opt_strategies = ARGV_ARRAY_INIT;
static struct argv_array opt_strategy_opts = ARGV_ARRAY_INIT;
static char *opt_gpg_sign;

/* Options passed to git-fetch */
static char *opt_all;
static char *opt_append;
static char *opt_upload_pack;
static int opt_force;
static char *opt_tags;
static char *opt_prune;
static char *opt_recurse_submodules;
static char *max_children;
static int opt_dry_run;
static char *opt_keep;
static char *opt_depth;
static char *opt_unshallow;
static char *opt_update_shallow;
static char *opt_refmap;

static struct option pull_options[] = {
	/* Shared options */
	OPT__VERBOSITY(&opt_verbosity),
	OPT_PASSTHRU(0, "progress", &opt_progress, NULL,
		N_("force progress reporting"),
		PARSE_OPT_NOARG),

	/* Options passed to git-merge or git-rebase */
	OPT_GROUP(N_("Options related to merging")),
	{ OPTION_CALLBACK, 'r', "rebase", &opt_rebase,
	  "false|true|preserve|interactive",
	  N_("incorporate changes by rebasing rather than merging"),
	  PARSE_OPT_OPTARG, parse_opt_rebase },
	OPT_PASSTHRU('n', NULL, &opt_diffstat, NULL,
		N_("do not show a diffstat at the end of the merge"),
		PARSE_OPT_NOARG | PARSE_OPT_NONEG),
	OPT_PASSTHRU(0, "stat", &opt_diffstat, NULL,
		N_("show a diffstat at the end of the merge"),
		PARSE_OPT_NOARG),
	OPT_PASSTHRU(0, "summary", &opt_diffstat, NULL,
		N_("(synonym to --stat)"),
		PARSE_OPT_NOARG | PARSE_OPT_HIDDEN),
	OPT_PASSTHRU(0, "log", &opt_log, N_("n"),
		N_("add (at most <n>) entries from shortlog to merge commit message"),
		PARSE_OPT_OPTARG),
	OPT_PASSTHRU(0, "squash", &opt_squash, NULL,
		N_("create a single commit instead of doing a merge"),
		PARSE_OPT_NOARG),
	OPT_PASSTHRU(0, "commit", &opt_commit, NULL,
		N_("perform a commit if the merge succeeds (default)"),
		PARSE_OPT_NOARG),
	OPT_PASSTHRU(0, "edit", &opt_edit, NULL,
		N_("edit message before committing"),
		PARSE_OPT_NOARG),
	OPT_PASSTHRU(0, "ff", &opt_ff, NULL,
		N_("allow fast-forward"),
		PARSE_OPT_NOARG),
	OPT_PASSTHRU(0, "ff-only", &opt_ff, NULL,
		N_("abort if fast-forward is not possible"),
		PARSE_OPT_NOARG | PARSE_OPT_NONEG),
	OPT_PASSTHRU(0, "verify-signatures", &opt_verify_signatures, NULL,
		N_("verify that the named commit has a valid GPG signature"),
		PARSE_OPT_NOARG),
	OPT_PASSTHRU_ARGV('s', "strategy", &opt_strategies, N_("strategy"),
		N_("merge strategy to use"),
		0),
	OPT_PASSTHRU_ARGV('X', "strategy-option", &opt_strategy_opts,
		N_("option=value"),
		N_("option for selected merge strategy"),
		0),
	OPT_PASSTHRU('S', "gpg-sign", &opt_gpg_sign, N_("key-id"),
		N_("GPG sign commit"),
		PARSE_OPT_OPTARG),

	/* Options passed to git-fetch */
	OPT_GROUP(N_("Options related to fetching")),
	OPT_PASSTHRU(0, "all", &opt_all, NULL,
		N_("fetch from all remotes"),
		PARSE_OPT_NOARG),
	OPT_PASSTHRU('a', "append", &opt_append, NULL,
		N_("append to .git/FETCH_HEAD instead of overwriting"),
		PARSE_OPT_NOARG),
	OPT_PASSTHRU(0, "upload-pack", &opt_upload_pack, N_("path"),
		N_("path to upload pack on remote end"),
		0),
	OPT__FORCE(&opt_force, N_("force overwrite of local branch")),
	OPT_PASSTHRU('t', "tags", &opt_tags, NULL,
		N_("fetch all tags and associated objects"),
		PARSE_OPT_NOARG),
	OPT_PASSTHRU('p', "prune", &opt_prune, NULL,
		N_("prune remote-tracking branches no longer on remote"),
		PARSE_OPT_NOARG),
	OPT_PASSTHRU(0, "recurse-submodules", &opt_recurse_submodules,
		N_("on-demand"),
		N_("control recursive fetching of submodules"),
		PARSE_OPT_OPTARG),
	OPT_PASSTHRU('j', "jobs", &max_children, N_("n"),
		N_("number of submodules pulled in parallel"),
		PARSE_OPT_OPTARG),
	OPT_BOOL(0, "dry-run", &opt_dry_run,
		N_("dry run")),
	OPT_PASSTHRU('k', "keep", &opt_keep, NULL,
		N_("keep downloaded pack"),
		PARSE_OPT_NOARG),
	OPT_PASSTHRU(0, "depth", &opt_depth, N_("depth"),
		N_("deepen history of shallow clone"),
		0),
	OPT_PASSTHRU(0, "unshallow", &opt_unshallow, NULL,
		N_("convert to a complete repository"),
		PARSE_OPT_NONEG | PARSE_OPT_NOARG),
	OPT_PASSTHRU(0, "update-shallow", &opt_update_shallow, NULL,
		N_("accept refs that update .git/shallow"),
		PARSE_OPT_NOARG),
	OPT_PASSTHRU(0, "refmap", &opt_refmap, N_("refmap"),
		N_("specify fetch refmap"),
		PARSE_OPT_NONEG),

	OPT_END()
};

/**
 * Pushes "-q" or "-v" switches into arr to match the opt_verbosity level.
 */
static void argv_push_verbosity(struct argv_array *arr)
{
	int verbosity;

	for (verbosity = opt_verbosity; verbosity > 0; verbosity--)
		argv_array_push(arr, "-v");

	for (verbosity = opt_verbosity; verbosity < 0; verbosity++)
		argv_array_push(arr, "-q");
}

/**
 * Pushes "-f" switches into arr to match the opt_force level.
 */
static void argv_push_force(struct argv_array *arr)
{
	int force = opt_force;
	while (force-- > 0)
		argv_array_push(arr, "-f");
}

/**
 * Sets the GIT_REFLOG_ACTION environment variable to the concatenation of argv
 */
static void set_reflog_message(int argc, const char **argv)
{
	int i;
	struct strbuf msg = STRBUF_INIT;

	for (i = 0; i < argc; i++) {
		if (i)
			strbuf_addch(&msg, ' ');
		strbuf_addstr(&msg, argv[i]);
	}

	setenv("GIT_REFLOG_ACTION", msg.buf, 0);

	strbuf_release(&msg);
}

/**
 * If pull.ff is unset, returns NULL. If pull.ff is "true", returns "--ff". If
 * pull.ff is "false", returns "--no-ff". If pull.ff is "only", returns
 * "--ff-only". Otherwise, if pull.ff is set to an invalid value, die with an
 * error.
 */
static const char *config_get_ff(void)
{
	const char *value;

	if (git_config_get_value("pull.ff", &value))
		return NULL;

	switch (git_config_maybe_bool("pull.ff", value)) {
	case 0:
		return "--no-ff";
	case 1:
		return "--ff";
	}

	if (!strcmp(value, "only"))
		return "--ff-only";

	die(_("Invalid value for pull.ff: %s"), value);
}

/**
 * Returns the default configured value for --rebase. It first looks for the
 * value of "branch.$curr_branch.rebase", where $curr_branch is the current
 * branch, and if HEAD is detached or the configuration key does not exist,
 * looks for the value of "pull.rebase". If both configuration keys do not
 * exist, returns REBASE_FALSE.
 */
static enum rebase_type config_get_rebase(void)
{
	struct branch *curr_branch = branch_get("HEAD");
	const char *value;

	if (curr_branch) {
		char *key = xstrfmt("branch.%s.rebase", curr_branch->name);

		if (!git_config_get_value(key, &value)) {
			enum rebase_type ret = parse_config_rebase(key, value, 1);
			free(key);
			return ret;
		}

		free(key);
	}

	if (!git_config_get_value("pull.rebase", &value))
		return parse_config_rebase("pull.rebase", value, 1);

	return REBASE_FALSE;
}

/**
 * Appends merge candidates from FETCH_HEAD that are not marked not-for-merge
 * into merge_heads.
 */
static void get_merge_heads(struct sha1_array *merge_heads)
{
	const char *filename = git_path("FETCH_HEAD");
	FILE *fp;
	struct strbuf sb = STRBUF_INIT;
	unsigned char sha1[GIT_SHA1_RAWSZ];

	if (!(fp = fopen(filename, "r")))
		die_errno(_("could not open '%s' for reading"), filename);
	while (strbuf_getline_lf(&sb, fp) != EOF) {
		if (get_sha1_hex(sb.buf, sha1))
			continue;  /* invalid line: does not start with SHA1 */
		if (starts_with(sb.buf + GIT_SHA1_HEXSZ, "\tnot-for-merge\t"))
			continue;  /* ref is not-for-merge */
		sha1_array_append(merge_heads, sha1);
	}
	fclose(fp);
	strbuf_release(&sb);
}

/**
 * Used by die_no_merge_candidates() as a for_each_remote() callback to
 * retrieve the name of the remote if the repository only has one remote.
 */
static int get_only_remote(struct remote *remote, void *cb_data)
{
	const char **remote_name = cb_data;

	if (*remote_name)
		return -1;

	*remote_name = remote->name;
	return 0;
}

/**
 * Dies with the appropriate reason for why there are no merge candidates:
 *
 * 1. We fetched from a specific remote, and a refspec was given, but it ended
 *    up not fetching anything. This is usually because the user provided a
 *    wildcard refspec which had no matches on the remote end.
 *
 * 2. We fetched from a non-default remote, but didn't specify a branch to
 *    merge. We can't use the configured one because it applies to the default
 *    remote, thus the user must specify the branches to merge.
 *
 * 3. We fetched from the branch's or repo's default remote, but:
 *
 *    a. We are not on a branch, so there will never be a configured branch to
 *       merge with.
 *
 *    b. We are on a branch, but there is no configured branch to merge with.
 *
 * 4. We fetched from the branch's or repo's default remote, but the configured
 *    branch to merge didn't get fetched. (Either it doesn't exist, or wasn't
 *    part of the configured fetch refspec.)
 */
static void NORETURN die_no_merge_candidates(const char *repo, const char **refspecs)
{
	struct branch *curr_branch = branch_get("HEAD");
	const char *remote = curr_branch ? curr_branch->remote_name : NULL;

	if (*refspecs) {
		if (opt_rebase)
			fprintf_ln(stderr, _("There is no candidate for rebasing against among the refs that you just fetched."));
		else
			fprintf_ln(stderr, _("There are no candidates for merging among the refs that you just fetched."));
		fprintf_ln(stderr, _("Generally this means that you provided a wildcard refspec which had no\n"
					"matches on the remote end."));
	} else if (repo && curr_branch && (!remote || strcmp(repo, remote))) {
		fprintf_ln(stderr, _("You asked to pull from the remote '%s', but did not specify\n"
			"a branch. Because this is not the default configured remote\n"
			"for your current branch, you must specify a branch on the command line."),
			repo);
	} else if (!curr_branch) {
		fprintf_ln(stderr, _("You are not currently on a branch."));
		if (opt_rebase)
			fprintf_ln(stderr, _("Please specify which branch you want to rebase against."));
		else
			fprintf_ln(stderr, _("Please specify which branch you want to merge with."));
		fprintf_ln(stderr, _("See git-pull(1) for details."));
		fprintf(stderr, "\n");
		fprintf_ln(stderr, "    git pull <remote> <branch>");
		fprintf(stderr, "\n");
	} else if (!curr_branch->merge_nr) {
		const char *remote_name = NULL;

		if (for_each_remote(get_only_remote, &remote_name) || !remote_name)
			remote_name = "<remote>";

		fprintf_ln(stderr, _("There is no tracking information for the current branch."));
		if (opt_rebase)
			fprintf_ln(stderr, _("Please specify which branch you want to rebase against."));
		else
			fprintf_ln(stderr, _("Please specify which branch you want to merge with."));
		fprintf_ln(stderr, _("See git-pull(1) for details."));
		fprintf(stderr, "\n");
		fprintf_ln(stderr, "    git pull <remote> <branch>");
		fprintf(stderr, "\n");
		fprintf_ln(stderr, _("If you wish to set tracking information for this branch you can do so with:\n"
				"\n"
				"    git branch --set-upstream-to=%s/<branch> %s\n"),
				remote_name, curr_branch->name);
	} else
		fprintf_ln(stderr, _("Your configuration specifies to merge with the ref '%s'\n"
			"from the remote, but no such ref was fetched."),
			*curr_branch->merge_name);
	exit(1);
}

/**
 * Parses argv into [<repo> [<refspecs>...]], returning their values in `repo`
 * as a string and `refspecs` as a null-terminated array of strings. If `repo`
 * is not provided in argv, it is set to NULL.
 */
static void parse_repo_refspecs(int argc, const char **argv, const char **repo,
		const char ***refspecs)
{
	if (argc > 0) {
		*repo = *argv++;
		argc--;
	} else
		*repo = NULL;
	*refspecs = argv;
}

/**
 * Runs git-fetch, returning its exit status. `repo` and `refspecs` are the
 * repository and refspecs to fetch, or NULL if they are not provided.
 */
static int run_fetch(const char *repo, const char **refspecs)
{
	struct argv_array args = ARGV_ARRAY_INIT;
	int ret;

	argv_array_pushl(&args, "fetch", "--update-head-ok", NULL);

	/* Shared options */
	argv_push_verbosity(&args);
	if (opt_progress)
		argv_array_push(&args, opt_progress);

	/* Options passed to git-fetch */
	if (opt_all)
		argv_array_push(&args, opt_all);
	if (opt_append)
		argv_array_push(&args, opt_append);
	if (opt_upload_pack)
		argv_array_push(&args, opt_upload_pack);
	argv_push_force(&args);
	if (opt_tags)
		argv_array_push(&args, opt_tags);
	if (opt_prune)
		argv_array_push(&args, opt_prune);
	if (opt_recurse_submodules)
		argv_array_push(&args, opt_recurse_submodules);
	if (max_children)
		argv_array_push(&args, max_children);
	if (opt_dry_run)
		argv_array_push(&args, "--dry-run");
	if (opt_keep)
		argv_array_push(&args, opt_keep);
	if (opt_depth)
		argv_array_push(&args, opt_depth);
	if (opt_unshallow)
		argv_array_push(&args, opt_unshallow);
	if (opt_update_shallow)
		argv_array_push(&args, opt_update_shallow);
	if (opt_refmap)
		argv_array_push(&args, opt_refmap);

	if (repo) {
		argv_array_push(&args, repo);
		argv_array_pushv(&args, refspecs);
	} else if (*refspecs)
		die("BUG: refspecs without repo?");
	ret = run_command_v_opt(args.argv, RUN_GIT_CMD);
	argv_array_clear(&args);
	return ret;
}

/**
 * "Pulls into void" by branching off merge_head.
 */
static int pull_into_void(const unsigned char *merge_head,
		const unsigned char *curr_head)
{
	/*
	 * Two-way merge: we treat the index as based on an empty tree,
	 * and try to fast-forward to HEAD. This ensures we will not lose
	 * index/worktree changes that the user already made on the unborn
	 * branch.
	 */
	if (checkout_fast_forward(EMPTY_TREE_SHA1_BIN, merge_head, 0))
		return 1;

	if (update_ref("initial pull", "HEAD", merge_head, curr_head, 0, UPDATE_REFS_DIE_ON_ERR))
		return 1;

	return 0;
}

/**
 * Runs git-merge, returning its exit status.
 */
static int run_merge(void)
{
	int ret;
	struct argv_array args = ARGV_ARRAY_INIT;

	argv_array_pushl(&args, "merge", NULL);

	/* Shared options */
	argv_push_verbosity(&args);
	if (opt_progress)
		argv_array_push(&args, opt_progress);

	/* Options passed to git-merge */
	if (opt_diffstat)
		argv_array_push(&args, opt_diffstat);
	if (opt_log)
		argv_array_push(&args, opt_log);
	if (opt_squash)
		argv_array_push(&args, opt_squash);
	if (opt_commit)
		argv_array_push(&args, opt_commit);
	if (opt_edit)
		argv_array_push(&args, opt_edit);
	if (opt_ff)
		argv_array_push(&args, opt_ff);
	if (opt_verify_signatures)
		argv_array_push(&args, opt_verify_signatures);
	argv_array_pushv(&args, opt_strategies.argv);
	argv_array_pushv(&args, opt_strategy_opts.argv);
	if (opt_gpg_sign)
		argv_array_push(&args, opt_gpg_sign);

	argv_array_push(&args, "FETCH_HEAD");
	ret = run_command_v_opt(args.argv, RUN_GIT_CMD);
	argv_array_clear(&args);
	return ret;
}

/**
 * Returns remote's upstream branch for the current branch. If remote is NULL,
 * the current branch's configured default remote is used. Returns NULL if
 * `remote` does not name a valid remote, HEAD does not point to a branch,
 * remote is not the branch's configured remote or the branch does not have any
 * configured upstream branch.
 */
static const char *get_upstream_branch(const char *remote)
{
	struct remote *rm;
	struct branch *curr_branch;
	const char *curr_branch_remote;

	rm = remote_get(remote);
	if (!rm)
		return NULL;

	curr_branch = branch_get("HEAD");
	if (!curr_branch)
		return NULL;

	curr_branch_remote = remote_for_branch(curr_branch, NULL);
	assert(curr_branch_remote);

	if (strcmp(curr_branch_remote, rm->name))
		return NULL;

	return branch_get_upstream(curr_branch, NULL);
}

/**
 * Derives the remote tracking branch from the remote and refspec.
 *
 * FIXME: The current implementation assumes the default mapping of
 * refs/heads/<branch_name> to refs/remotes/<remote_name>/<branch_name>.
 */
static const char *get_tracking_branch(const char *remote, const char *refspec)
{
	struct refspec *spec;
	const char *spec_src;
	const char *merge_branch;

	spec = parse_fetch_refspec(1, &refspec);
	spec_src = spec->src;
	if (!*spec_src || !strcmp(spec_src, "HEAD"))
		spec_src = "HEAD";
	else if (skip_prefix(spec_src, "heads/", &spec_src))
		;
	else if (skip_prefix(spec_src, "refs/heads/", &spec_src))
		;
	else if (starts_with(spec_src, "refs/") ||
		starts_with(spec_src, "tags/") ||
		starts_with(spec_src, "remotes/"))
		spec_src = "";

	if (*spec_src) {
		if (!strcmp(remote, "."))
			merge_branch = mkpath("refs/heads/%s", spec_src);
		else
			merge_branch = mkpath("refs/remotes/%s/%s", remote, spec_src);
	} else
		merge_branch = NULL;

	free_refspec(1, spec);
	return merge_branch;
}

/**
 * Given the repo and refspecs, sets fork_point to the point at which the
 * current branch forked from its remote tracking branch. Returns 0 on success,
 * -1 on failure.
 */
static int get_rebase_fork_point(unsigned char *fork_point, const char *repo,
		const char *refspec)
{
	int ret;
	struct branch *curr_branch;
	const char *remote_branch;
	struct child_process cp = CHILD_PROCESS_INIT;
	struct strbuf sb = STRBUF_INIT;

	curr_branch = branch_get("HEAD");
	if (!curr_branch)
		return -1;

	if (refspec)
		remote_branch = get_tracking_branch(repo, refspec);
	else
		remote_branch = get_upstream_branch(repo);

	if (!remote_branch)
		return -1;

	argv_array_pushl(&cp.args, "merge-base", "--fork-point",
			remote_branch, curr_branch->name, NULL);
	cp.no_stdin = 1;
	cp.no_stderr = 1;
	cp.git_cmd = 1;

	ret = capture_command(&cp, &sb, GIT_SHA1_HEXSZ);
	if (ret)
		goto cleanup;

	ret = get_sha1_hex(sb.buf, fork_point);
	if (ret)
		goto cleanup;

cleanup:
	strbuf_release(&sb);
	return ret ? -1 : 0;
}

/**
 * Sets merge_base to the octopus merge base of curr_head, merge_head and
 * fork_point. Returns 0 if a merge base is found, 1 otherwise.
 */
static int get_octopus_merge_base(unsigned char *merge_base,
		const unsigned char *curr_head,
		const unsigned char *merge_head,
		const unsigned char *fork_point)
{
	struct commit_list *revs = NULL, *result;

	commit_list_insert(lookup_commit_reference(curr_head), &revs);
	commit_list_insert(lookup_commit_reference(merge_head), &revs);
	if (!is_null_sha1(fork_point))
		commit_list_insert(lookup_commit_reference(fork_point), &revs);

	result = reduce_heads(get_octopus_merge_bases(revs));
	free_commit_list(revs);
	if (!result)
		return 1;

	hashcpy(merge_base, result->item->object.oid.hash);
	return 0;
}

/**
 * Given the current HEAD SHA1, the merge head returned from git-fetch and the
 * fork point calculated by get_rebase_fork_point(), runs git-rebase with the
 * appropriate arguments and returns its exit status.
 */
static int run_rebase(const unsigned char *curr_head,
		const unsigned char *merge_head,
		const unsigned char *fork_point)
{
	int ret;
	unsigned char oct_merge_base[GIT_SHA1_RAWSZ];
	struct argv_array args = ARGV_ARRAY_INIT;

	if (!get_octopus_merge_base(oct_merge_base, curr_head, merge_head, fork_point))
		if (!is_null_sha1(fork_point) && !hashcmp(oct_merge_base, fork_point))
			fork_point = NULL;

	argv_array_push(&args, "rebase");

	/* Shared options */
	argv_push_verbosity(&args);

	/* Options passed to git-rebase */
	if (opt_rebase == REBASE_PRESERVE)
		argv_array_push(&args, "--preserve-merges");
	else if (opt_rebase == REBASE_INTERACTIVE)
		argv_array_push(&args, "--interactive");
	if (opt_diffstat)
		argv_array_push(&args, opt_diffstat);
	argv_array_pushv(&args, opt_strategies.argv);
	argv_array_pushv(&args, opt_strategy_opts.argv);
	if (opt_gpg_sign)
		argv_array_push(&args, opt_gpg_sign);

	argv_array_push(&args, "--onto");
	argv_array_push(&args, sha1_to_hex(merge_head));

	if (fork_point && !is_null_sha1(fork_point))
		argv_array_push(&args, sha1_to_hex(fork_point));
	else
		argv_array_push(&args, sha1_to_hex(merge_head));

	ret = run_command_v_opt(args.argv, RUN_GIT_CMD);
	argv_array_clear(&args);
	return ret;
}

int cmd_pull(int argc, const char **argv, const char *prefix)
{
	const char *repo, **refspecs;
	struct sha1_array merge_heads = SHA1_ARRAY_INIT;
	unsigned char orig_head[GIT_SHA1_RAWSZ], curr_head[GIT_SHA1_RAWSZ];
	unsigned char rebase_fork_point[GIT_SHA1_RAWSZ];

	if (!getenv("GIT_REFLOG_ACTION"))
		set_reflog_message(argc, argv);

	argc = parse_options(argc, argv, prefix, pull_options, pull_usage, 0);

	parse_repo_refspecs(argc, argv, &repo, &refspecs);

	if (!opt_ff)
		opt_ff = xstrdup_or_null(config_get_ff());

	if (opt_rebase < 0)
		opt_rebase = config_get_rebase();

	git_config(git_default_config, NULL);

	if (read_cache_unmerged())
		die_resolve_conflict("Pull");

	if (file_exists(git_path("MERGE_HEAD")))
		die_conclude_merge();

	if (get_sha1("HEAD", orig_head))
		hashclr(orig_head);

	if (opt_rebase) {
		int autostash = 0;

		if (is_null_sha1(orig_head) && !is_cache_unborn())
			die(_("Updating an unborn branch with changes added to the index."));

		git_config_get_bool("rebase.autostash", &autostash);
		if (!autostash)
			require_clean_work_tree("pull with rebase",
				"Please commit or stash them.", 0);

		if (get_rebase_fork_point(rebase_fork_point, repo, *refspecs))
			hashclr(rebase_fork_point);
	}

	if (run_fetch(repo, refspecs))
		return 1;

	if (opt_dry_run)
		return 0;

	if (get_sha1("HEAD", curr_head))
		hashclr(curr_head);

	if (!is_null_sha1(orig_head) && !is_null_sha1(curr_head) &&
			hashcmp(orig_head, curr_head)) {
		/*
		 * The fetch involved updating the current branch.
		 *
		 * The working tree and the index file are still based on
		 * orig_head commit, but we are merging into curr_head.
		 * Update the working tree to match curr_head.
		 */

		warning(_("fetch updated the current branch head.\n"
			"fast-forwarding your working tree from\n"
			"commit %s."), sha1_to_hex(orig_head));

		if (checkout_fast_forward(orig_head, curr_head, 0))
			die(_("Cannot fast-forward your working tree.\n"
				"After making sure that you saved anything precious from\n"
				"$ git diff %s\n"
				"output, run\n"
				"$ git reset --hard\n"
				"to recover."), sha1_to_hex(orig_head));
	}

	get_merge_heads(&merge_heads);

	if (!merge_heads.nr)
		die_no_merge_candidates(repo, refspecs);

	if (is_null_sha1(orig_head)) {
		if (merge_heads.nr > 1)
			die(_("Cannot merge multiple branches into empty head."));
		return pull_into_void(*merge_heads.sha1, curr_head);
	} else if (opt_rebase) {
		if (merge_heads.nr > 1)
			die(_("Cannot rebase onto multiple branches."));
		return run_rebase(curr_head, *merge_heads.sha1, rebase_fork_point);
	} else
		return run_merge();
}
