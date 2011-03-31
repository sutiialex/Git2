#include <git2.h>
#include "git-compat-util.h"
#include "exec_cmd.h"

/*
 * A signature file has a very simple fixed format: four lines
 * of "object <sha1>" + "type <typename>" + "tag <tagname>" +
 * "tagger <committer>", followed by a blank line, a free-form tag
 * message and a signature block that git itself doesn't care about,
 * but that can be verified with gpg or similar.
 *
 * The first four lines are guaranteed to be at least 83 bytes:
 * "object <sha1>\n" is 48 bytes, "type tag\n" at 9 bytes is the
 * shortest possible type-line, "tag .\n" at 6 bytes is the shortest
 * single-character-tag line, and "tagger . <> 0 +0000\n" at 20 bytes is
 * the shortest possible tagger-line.
 */

#define BUF_LEN   4096
#define TAG_NAME_LEN 40 /* I couldn't find out what is the max permited length for the tag name */

/*
 * We refuse to tag something we can't verify. Just because.
 */
static int verify_object(git_odb *odb, const char *expected_type, git_oid *oid, git_otype *type)
{
	git_odb_object *obj;
	const char *str_type;
	
	/* Get the object from database */
	if (git_odb_read(&obj, odb, oid))
		return error("Object not found");

	/* Get the type */
	*type = git_odb_object_type(obj);
	git_odb_object_close(obj);
	str_type = git_object_type2string(*type);

	if (memcmp(str_type, expected_type, strlen(str_type)))
		return -1;

	return 0;
}

static int verify_and_create_tag(git_repository *repo, char *buffer, unsigned long size)
{
	int typelen;
	char type[20];
	char tagname[TAG_NAME_LEN];
	char author_name[40];
	char author_email[40];
	char timestamp[20];
	char offset[10];
	char *tagname_begin;
	char sha1_hex[40];
	char *object, *type_line, *tag_line, *tagger_line, *lb, *rb;
	size_t len;
	git_oid target_oid, tag_oid;
	git_otype target_type;
	git_odb *odb;

	if (size < 84)
		return error("wanna fool me ? you obviously got the size wrong !");

	buffer[size] = 0;

	/* Verify object line */
	object = buffer;
	if (memcmp(object, "object ", 7))
		return error("char%d: does not start with \"object \"", 0);

	/* Get the sha and create the oid */
	memcpy(sha1_hex, object+7, 40);
	if (git_oid_mkstr(&target_oid, sha1_hex) != GIT_SUCCESS)
		return error("char%d: Invalid SHA1 hash", 7);

	/* Verify type line */
	type_line = object + 48;
	if (memcmp(type_line - 1, "\ntype ", 6))
		return error("char%d: could not find \"\\ntype \"", 47);

	/* Verify tag-line */
	tag_line = strchr(type_line, '\n');
	if (!tag_line)
		return error("char%"PRIuMAX": could not find next \"\\n\"",
				(uintmax_t) (type_line - buffer));
	tag_line++;
	if (memcmp(tag_line, "tag ", 4) || tag_line[4] == '\n')
		return error("char%"PRIuMAX": no \"tag \" found",
				(uintmax_t) (tag_line - buffer));

	/* Get the actual type */
	typelen = tag_line - type_line - strlen("type \n");
	if (typelen >= sizeof(type))
		return error("char%"PRIuMAX": type too long",
				(uintmax_t) (type_line+5 - buffer));

	memcpy(type, type_line+5, typelen);
	type[typelen] = 0;

	odb = git_repository_database(repo);
	if (odb == NULL)
		return error("Could not get the database");

	/* Verify that the object matches */
	if (verify_object(odb, type, &target_oid, &target_type))
		return error("char%d: could not verify object %s", 7, sha1_hex);

	/* Verify the tag-name: we don't allow control characters or spaces in it */
	tag_line += 4;
	tagname_begin = tag_line;
	for (;;) {
		unsigned char c = *tag_line++;
		if (c == '\n')
			break;
		if (c > ' ')
			continue;
		return error("char%"PRIuMAX": could not verify tag name",
				(uintmax_t) (tag_line - buffer));
	}

	/* Save the tag name */
	if (tag_line - 1 - tagname_begin  >= TAG_NAME_LEN)
		return error("Tag name too long");
	memcpy(tagname, tagname_begin, tag_line - 1 - tagname_begin);

	/* Verify the tagger line */
	tagger_line = tag_line;

	if (memcmp(tagger_line, "tagger ", 7))
		return error("char%"PRIuMAX": could not find \"tagger \"",
			(uintmax_t) (tagger_line - buffer));

	/*
	 * Check for correct form for name and email
	 * i.e. " <" followed by "> " on _this_ line
	 * No angle brackets within the name or email address fields.
	 * No spaces within the email address field.
	 */
	tagger_line += 7;
	if (!(lb = strstr(tagger_line, " <")) || !(rb = strstr(lb+2, "> ")) ||
		strpbrk(tagger_line, "<>\n") != lb+1 ||
		strpbrk(lb+2, "><\n ") != rb)
		return error("char%"PRIuMAX": malformed tagger field",
			(uintmax_t) (tagger_line - buffer));

	/* Check for author name, at least one character, space is acceptable */
	if (lb == tagger_line)
		return error("char%"PRIuMAX": missing tagger name",
			(uintmax_t) (tagger_line - buffer));

	/* Copy the name */
	memcpy(author_name, tagger_line, lb - tagger_line);
	author_name[lb-tagger_line] = '\0';
	/* Copy the email */
	memcpy(author_email, lb+2, rb - lb - 2);
	author_email[rb-lb-2] = '\0';

	/* timestamp, 1 or more digits followed by space */
	tagger_line = rb + 2;
	if (!(len = strspn(tagger_line, "0123456789")))
		return error("char%"PRIuMAX": missing tag timestamp",
			(uintmax_t) (tagger_line - buffer));
	/* Copy the timestamp */
	memcpy(timestamp, tagger_line, len);
	timestamp[len] = '\0';
	tagger_line += len;
	if (*tagger_line != ' ')
		return error("char%"PRIuMAX": malformed tag timestamp",
			(uintmax_t) (tagger_line - buffer));
	tagger_line++;

	/* timezone, 5 digits [+-]hhmm, max. 1400 */
	if (!((tagger_line[0] == '+' || tagger_line[0] == '-') &&
		  strspn(tagger_line+1, "0123456789") == 4 &&
		  tagger_line[5] == '\n' && atoi(tagger_line+1) <= 1400))
		return error("char%"PRIuMAX": malformed tag timezone",
			(uintmax_t) (tagger_line - buffer));
	 /* Copy the timezone */
	memcpy(offset, tagger_line, 5);
	offset[5] = '\0';
	
	tagger_line += 6;

	/* Verify the blank line separating the header from the body */
	if (*tagger_line != '\n')
		return error("char%"PRIuMAX": trailing garbage in tag header",
			(uintmax_t) (tagger_line - buffer));

	/* Create the tag */
	git_signature *tagger;
	if ((tagger = git_signature_new(author_name, author_email, atoi(timestamp), atoi(offset))) == NULL)
		return error("Could create the signature");
	if (git_tag_create(&tag_oid, repo, tagname, &target_oid, target_type, tagger, tagger_line+1))
		return error("Could not create the tag");

	char out[41];
	out[40] = 0;
	git_oid_fmt(out, &tag_oid);
	printf("Tag sha1: %s\n", out);

	return 0;
}

int cmd_mktag(int argc, const char **argv)
{
	char buf[BUF_LEN];
	int read_no;
	char *pos;
	int count = BUF_LEN;
	git_repository *repo;

	if (argc != 0)
		usage("git mktag < signaturefile");

	/* Read the input into buffer */
	pos = buf;
	while (count > 0 && (read_no = read(0, pos, count)) > 0)
	{
		pos += read_no;
		count -= read_no;
	}

	if (read_no < 0)
		error("Could not read from stdin.");

	if (count == 0)
		warning("Could not read the whole input. The buffer is full.");
	*pos = '\0';
	count = pos - buf;

	/* Open the repo */
	if (git_repository_open(&repo, ".git"))
		return error("Could not open repository");

	/* Verify the input buffer for some basic sanity: it needs to start with
	   "object <sha1>\ntype\ntagger " */
	if (verify_and_create_tag(repo, buf, count) < 0)
		die("invalid tag signature file");

	git_repository_free(repo);
	return 0;
}
