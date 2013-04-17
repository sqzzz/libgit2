/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "blame.h"
#include "git2/commit.h"
#include "git2/revparse.h"
#include "git2/revwalk.h"
#include "git2/tree.h"
#include "git2/diff.h"
#include "util.h"
#include "repository.h"


static int hunk_byline_search_cmp(const void *key, const void *entry)
{
	uint32_t lineno = *(size_t*)key;
	git_blame_hunk *hunk = (git_blame_hunk*)entry;

	if (lineno < hunk->final_start_line_number)
		return -1;
	if (lineno > (hunk->final_start_line_number + hunk->lines_in_hunk))
		return 1;
	return 0;
}

static int hunk_sort_cmp_by_start_line(const void *_a, const void *_b)
{
	git_blame_hunk *a = (git_blame_hunk*)_a,
						*b = (git_blame_hunk*)_b;

	return a->final_start_line_number - b->final_start_line_number;
}

git_blame* git_blame__alloc(
	git_repository *repo,
	git_blame_options opts,
	const char *path)
{
	git_blame *gbr = (git_blame*)calloc(1, sizeof(git_blame));
	if (!gbr) {
		giterr_set_oom();
		return NULL;
	}
	git_vector_init(&gbr->hunks, 8, hunk_sort_cmp_by_start_line);
	gbr->repository = repo;
	gbr->options = opts;
	gbr->path = git__strdup(path);
	gbr->lines = NULL;
	return gbr;
}

void git_blame_free(git_blame *blame)
{
	size_t i;
	git_blame_hunk *hunk;

	if (!blame) return;

	git_vector_foreach(&blame->hunks, i, hunk) {
		git__free((char*)hunk->orig_path);
		git__free(hunk);
	}
	git_vector_free(&blame->hunks);

	git__free(blame->lines);
	git__free((void*)blame->path);
	git__free(blame);
}

uint32_t git_blame_get_hunk_count(git_blame *blame)
{
	assert(blame);
	return blame->hunks.length;
}

const git_blame_hunk *git_blame_get_hunk_byindex(git_blame *blame, uint32_t index)
{
	assert(blame);
	return (git_blame_hunk*)git_vector_get(&blame->hunks, index);
}

const git_blame_hunk *git_blame_get_hunk_byline(git_blame *blame, uint32_t lineno)
{
	size_t i;
	assert(blame);

	if (!git_vector_bsearch2( &i, &blame->hunks, hunk_byline_search_cmp, &lineno)) {
		return git_blame_get_hunk_byindex(blame, i);
	}

	return NULL;
}

static void normalize_options(
		git_blame_options *out,
		const git_blame_options *in,
		git_repository *repo)
{
	git_blame_options dummy = GIT_BLAME_OPTIONS_INIT;
	if (!in) in = &dummy;

	memmove(out, in, sizeof(git_blame_options));

	/* No newest_commit => HEAD */
	if (git_oid_iszero(&out->newest_commit)) {
		git_object *obj;
		git_revparse_single(&obj, repo, "HEAD");
		git_oid_cpy(&out->newest_commit, git_object_id(obj));
		git_object_free(obj);
	}
}

static void dump_lines(git_blame *blame)
{
	size_t i;
	for (i=0; i<blame->num_lines; i++) {
		char sha[41] = {0};
		git_blame__line *line = &blame->lines[i];
		git_oid_fmt(sha, &line->origin_oid);
		printf("%ld (%d) %s\n", i+1, line->tracked_line_number, sha);
	}
}

static void adjust_tracked_lines(git_blame *blame, char line_origin)
{
	size_t i;
	int adj;

	switch (line_origin)
	{
	case GIT_DIFF_LINE_ADDITION:    adj = -1; break;
	case GIT_DIFF_LINE_DELETION:    adj =  1; break;
	default: return;
	}

	for (i=blame->current_line; i<blame->num_lines; i++) {
		blame->lines[i].tracked_line_number += adj;
	}

	dump_lines(blame);
}

/*******************************************************************************
 * Trivial blaming
 *******************************************************************************/

static int trivial_file_cb(
	const git_diff_delta *delta,
	float progress,
	void *payload)
{
	GIT_UNUSED(delta);
	GIT_UNUSED(progress);
	GIT_UNUSED(payload);
	return 0;
}

static int trivial_hunk_cb(
	const git_diff_delta *delta,
	const git_diff_range *range,
	const char *header,
	size_t header_len,
	void *payload)
{
	git_blame *blame = (git_blame*)payload;

	GIT_UNUSED(delta);
	GIT_UNUSED(range);
	GIT_UNUSED(header);
	GIT_UNUSED(header_len);

	printf("  Hunk: %s (%d-%d) -> %s (%d-%d)\n",
			delta->old_file.path,
			range->old_start, range->old_start + range->old_lines,
			delta->new_file.path,
			range->new_start, range->new_start + range->new_lines);
	blame->current_line = range->new_start;
	return 0;
}

static git_blame__line* find_line_by_tracked_number(git_blame *blame)
{
	size_t i;

	for (i=0; i<blame->num_lines; i++)
	{
		if (blame->lines[i].tracked_line_number == blame->current_line &&
		    git_oid_iszero(&blame->lines[i].origin_oid))
			return &blame->lines[i];
	}
	return NULL;
}

static int trivial_line_cb(
	const git_diff_delta *delta,
	const git_diff_range *range,
	char line_origin,
	const char *content,
	size_t content_len,
	void *payload)
{
	char buf[1024] = {0};
	git_blame *blame = (git_blame*)payload;
	git_blame__line *line = find_line_by_tracked_number(blame);

	GIT_UNUSED(range);

	strncpy(buf, content, content_len-1);
	printf("    %c %s\n", line_origin, buf);

	if (line_origin == GIT_DIFF_LINE_ADDITION &&
		 line &&
	    git_oid_iszero(&line->origin_oid)) {
		git_oid_cpy(&line->origin_oid, &blame->current_commit);
		printf("Marked!\n");
	}

	adjust_tracked_lines(blame, line_origin);
	blame->current_line++;

	return 0;
}

static int trivial_match(git_diff_list *diff, git_blame *blame)
{
	return git_diff_foreach(diff, trivial_file_cb, trivial_hunk_cb, trivial_line_cb, blame);
}

static int walk_and_mark(git_blame *blame, git_revwalk *walk)
{
	git_oid oid;
	int error;

	while (!(error = git_revwalk_next(&oid, walk))) {
		git_commit *commit = NULL,
					  *parent = NULL;
		git_tree *committree = NULL,
					*parenttree = NULL;
		git_diff_list *diff = NULL;
		git_diff_options diffopts = GIT_DIFF_OPTIONS_INIT;
		git_diff_find_options diff_find_opts = GIT_DIFF_FIND_OPTIONS_INIT;
		char *paths[1];

		if ((error = git_commit_lookup(&commit, blame->repository, &oid)) < 0)
			break;

		/* Don't consider merge commits */
		/* TODO: git_diff_merge? */
		if (git_commit_parentcount(commit) > 1)
			continue;

		error = git_commit_parent(&parent, commit, 0);
		if (error != 0 && error != GIT_ENOTFOUND)
			goto cleanup;

		/* Get the trees from this commit and its parent */
		if ((error = git_commit_tree(&committree, commit)) < 0)
			goto cleanup;
		if (parent && ((error = git_commit_tree(&parenttree, parent)) < 0))
			goto cleanup;

		/* Configure the diff */
		diffopts.context_lines = 0;
		strcpy(paths[0], blame->path);
		diffopts.pathspec.strings = paths;
		diffopts.pathspec.count = 1;

		/* Generate a diff between the two trees */
		if ((error = git_diff_tree_to_tree(&diff, blame->repository, parenttree, committree, &diffopts)) < 0)
			goto cleanup;

		/* Let diff find file moves */
		if (blame->options.flags & GIT_BLAME_TRACK_FILE_RENAMES)
			if ((error = git_diff_find_similar(diff, &diff_find_opts)) < 0)
				goto cleanup;

		git_oid_cpy(&blame->current_commit, &oid);

		/* Trivial matching */
		{
			char str[41] = {0};
			git_oid_fmt(str, &oid);
			printf("Rev %s\n", str);
		}
		if ((error = trivial_match(diff, blame)) < 0)
			goto cleanup;

cleanup:
		git_tree_free(committree);
		git_tree_free(parenttree);
		git_commit_free(commit);
		git_commit_free(parent);
		git_diff_list_free(diff);
		if (error != 0) break;
	}

	if (error == GIT_ITEROVER)
		error = 0;
	return error;
}

static int get_line_count(size_t *out, git_repository *repo, git_oid *commit_id, const char *path)
{
	int retval = -1;
	git_commit *commit = NULL;
	git_tree *tree = NULL;
	git_tree_entry *tree_entry = NULL;
	git_object *obj = NULL;

	if (git_commit_lookup(&commit, repo, commit_id) < 0 ||
	    git_commit_tree(&tree, commit) < 0 ||
	    git_tree_entry_bypath(&tree_entry, tree, path) < 0 ||
	    git_tree_entry_to_object(&obj, repo, tree_entry) < 0 ||
	    git_object_type(obj) != GIT_OBJ_BLOB)
		goto cleanup;

	{
		size_t count = 0;
		const char *str = git_blob_rawcontent((git_blob*)obj);
		while ((str = strchr(str+1, '\n')) != NULL)
			count++;
		if (out) *out = count;
		retval = 0;
	}
	printf("%s has %d lines\n", path, *out);

cleanup:
	git_object_free(obj);
	git_tree_entry_free(tree_entry);
	git_tree_free(tree);
	git_commit_free(commit);
	return retval;
}

static int get_line_count(size_t *out, git_repository *repo, git_oid *commit_id, const char *path)
{
	int retval = -1;
	git_commit *commit = NULL;
	git_tree *tree = NULL;
	git_tree_entry *tree_entry = NULL;
	git_object *obj = NULL;

	if (git_commit_lookup(&commit, repo, commit_id) < 0 ||
	    git_commit_tree(&tree, commit) < 0 ||
	    git_tree_entry_bypath(&tree_entry, tree, path) < 0 ||
	    git_tree_entry_to_object(&obj, repo, tree_entry) < 0 ||
	    git_object_type(obj) != GIT_OBJ_BLOB)
		goto cleanup;

	{
		size_t count = 0;
		const char *str = git_blob_rawcontent((git_blob*)obj);
		while ((str = strchr(str+1, '\n')) != NULL)
			count++;
		if (out) *out = count;
		retval = 0;
	}
	printf("%s has %zd lines\n", path, *out);

cleanup:
	git_object_free(obj);
	git_tree_entry_free(tree_entry);
	git_tree_free(tree);
	git_commit_free(commit);
	return retval;
}

>>>>>>> deed710... Some progress toward trivial blame
int git_blame_file(
		git_blame **out,
		git_repository *repo,
		const char *path,
		git_blame_options *options)
{
	int error = -1;
	size_t i;
	git_blame_options normOptions = GIT_BLAME_OPTIONS_INIT;
	git_blame *blame = NULL;
	git_revwalk *walk = NULL;

	if (!out || !repo || !path) return -1;
	normalize_options(&normOptions, options, repo);

	blame = git_blame__alloc(repo, normOptions, path);
	if (!blame) return -1;

	/* Set up the revwalk */
	if ((error = git_revwalk_new(&walk, repo)) < 0 ||
		 (error = git_revwalk_push(walk, &normOptions.newest_commit)) < 0)
		goto on_error;
	if (!git_oid_iszero(&normOptions.oldest_commit) &&
		 (error = git_revwalk_hide(walk, &normOptions.oldest_commit)) < 0)
		goto on_error;
	git_revwalk_sorting(walk, GIT_SORT_TIME);

	if ((error = get_line_count(&blame->num_lines, repo, &normOptions.newest_commit, path)) < 0)
		goto on_error;
	blame->lines = git__calloc(blame->num_lines, sizeof(git_blame__line));
	if (!blame->lines) goto on_error;
	for (i=0; i<blame->num_lines; i++)
		blame->lines[i].tracked_line_number = i+1;

	if ((error = walk_and_mark(blame, walk)) < 0)
		goto on_error;

	dump_lines(blame);

	git_revwalk_free(walk);
	*out = blame;
	return 0;

on_error:
	git_revwalk_free(walk);
	git_blame_free(blame);
	return error;
}

int git_blame_buffer(
		git_blame **out,
		git_blame *reference,
		const char *buffer,
		size_t buffer_len)
{
	git_blame *blame;

	if (!out || !reference || !buffer || !buffer_len)
		return -1;

	blame = git_blame__alloc(reference->repository, reference->options, reference->path);

	*out = blame;
	return 0;
}
