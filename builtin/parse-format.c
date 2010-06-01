#include "builtin.h"
#include "strbuf.h"
#include "utf8.h"
#include "parse-options.h"

/*
I want to parse this:

"commit %(abbrev-commit? %h : %H)%(decorate?" (%d)")%n"
"%(merge ? "Merge: %(abbrev-commit?%p:%P)%n")"
"Author: %an <%ae>%n"
"Date: %ad%n"
"%n"
"    %s%n"
"%n"
"%w(0,4,4)%b%w%n"
"%(notes? "%nNotes:%n%w(0,4,4)%N%w%n)"

merge
opt-abbrev-commit
opt-decorate
has-decorate
decorate (= opt-decorate && has-decorate)
indent
notes
notes(refspec)
notes("refspec")



parse into a tree of:
	[
		literal
		{
			condition,
			[
				literal,
				COMMIT_HASH,
				literal,
				DECORATE
			]
		}
	]


	foobar%("baz")
	foobar%("baz%(bash ? ")
	                     ^- what's this mean?
			     	well, it's a " with no end-quote, so %(bash...) returns NULL
				so %(bash is literal, so the " means the end of %("baz

				So, do we need to know at the point of "%(bash ?" that we are
				already within a quote? no.

	foobar%(bar ? %(baz)
	                   ^- here we complete %(baz, but %(bar? is incomplete. So, %(bar is taken
			      as literal, and %(baz) is parsed again

			      Perhaps the rule is "if ambiguous, the shortest paren will win"?

	foobar%(bar ? baz ? bam)
			  ^- this is a problem, and we should not allow it
	foobar%(bar ? baz : baz : baz )
	                        ^- this is a problem too

	bleh%("foobar%(foo"  bleh)"
*/

static const char * const builtin_parse_format_usage[] = {
	"git parse-format <format-string>",
	NULL
};

enum format_part_type {
	FORMAT_PART_UNKNOWN,
	FORMAT_PART_FORMAT,
	FORMAT_PART_LITERAL,
	FORMAT_PART_COMMIT_HASH,
	FORMAT_PART_COMMIT_HASH_ABBREV,
	FORMAT_PART_PARENT_HASHES,
	FORMAT_PART_PARENT_HASHES_ABBREV,

	FORMAT_PART_CONDITION_MERGE
};

struct format_part;
struct format_parts {
	char	*format;
	size_t	format_len;
	size_t len;
	size_t alloc;
	struct format_part	*part;
};

struct format_part {
	enum format_part_type	type;
	char			*format;
	size_t			format_len;
	char			*literal;
	struct format_parts	*args;
	struct format_parts	*parts;
	struct format_parts	*alt_parts;
};

struct format_parse_state {
//	format_parse_state *parent;
	int	expect_quote;
	int	expect_colon; // implies "error on question"
	int	expect_paren; // if !expect_colon, error when colon found. implies error on question.
	int	ignore_space;

	char	found;
};

struct format_parts *parse_format(const char *unparsed,
				  struct format_parse_state *state);
#define format_parts_alloc() \
	((struct format_parts*)calloc(1, sizeof(struct format_parts)))
#define format_part_alloc() \
	((struct format_part*)calloc(1, sizeof(struct format_part)))
void format_part_free(struct format_part **part);
void format_parts_free(struct format_parts **parts)
{
	if((*parts)->part)
		free((*parts)->part);
	free(*parts);
	*parts = NULL;
}
void format_part_free(struct format_part **part)
{
	if ((*part)->format)
		free((*part)->format);
	if ((*part)->literal)
		free((*part)->literal);
//	if ((*part)->args)
//		format_parts_free((*part)->args);
	if ((*part)->parts)
		format_parts_free(&(*part)->parts);
	if ((*part)->alt_parts)
		format_parts_free(&(*part)->alt_parts);
	free(*part);
	*part = NULL;
}

static struct format_part * parts_add(struct format_parts *parts,
				      enum format_part_type type)
{
	ALLOC_GROW(parts->part, parts->len+1, parts->alloc);
	memset(&parts->part[parts->len], 0, sizeof(parts->part[parts->len]));
	parts->part[parts->len].type = type;
	parts->len++;
	return &parts->part[parts->len-1];
}

static struct format_part * parts_add_part(struct format_parts *parts,
					   struct format_part *part)
{
	struct format_part *dst = parts_add(parts, FORMAT_PART_UNKNOWN);
	memcpy(dst, part, sizeof(*dst));
	return dst;
}

static void parts_add_nliteral(struct format_parts *parts, const char *literal,
			       size_t len)
{
	if( len == 0 ) return;
	parts_add(parts, FORMAT_PART_LITERAL);
	parts->part[parts->len-1].literal = xstrndup(literal, len);
	return;
}

static void parts_add_literal(struct format_parts *parts, const char *literal)
{
	parts_add(parts, FORMAT_PART_LITERAL);
	parts->part[parts->len-1].literal = xstrdup(literal);
	return;
}

static struct strbuf * parts_debug(struct format_parts *parts, size_t indent)
{
	struct format_part *part;
	struct strbuf *buf = xcalloc(1, sizeof(*buf));
	struct strbuf *otherbuf;
	struct {enum format_part_type type; char *label;} labels[] = {
			{FORMAT_PART_FORMAT, "FORMAT"},
			{FORMAT_PART_LITERAL, "LITERAL"},
			{FORMAT_PART_COMMIT_HASH, "COMMIT-HASH"},
			{FORMAT_PART_COMMIT_HASH_ABBREV, "COMMIT-HASH-ABBREV"},
			{FORMAT_PART_PARENT_HASHES, "PARENT-HASHES"},
			{FORMAT_PART_PARENT_HASHES_ABBREV, "PARENT-HASHES-ABBREV"},
			{FORMAT_PART_CONDITION_MERGE, "CONDITION:MERGE"}
		};
	char *label;
	size_t i,j;
	strbuf_init(buf, 0);

	strbuf_add_wrapped_text(buf, "{[PARTS:", indent++, 0, 0);
	strbuf_addf(buf, "%d]\n", parts->len);
	for (i = 0; i < parts->len; i++) {
		part = &parts->part[i];
		label = "UNKNOWN";
		for (j = 0; j < ARRAY_SIZE(labels); j++) {
			if (labels[j].type == part->type) {
				label = labels[j].label;
			}
		}

		strbuf_add_wrapped_text(buf, "{", indent, 0, 0);
		strbuf_addstr(buf, label);
		if (part->literal) {
			strbuf_addstr(buf, " ");
			strbuf_add_wrapped_text(buf, part->literal,
						0, indent+strlen(label)+1, 0);
		}

		if (part->parts) {
			strbuf_addstr(buf, "\n");
			strbuf_add_wrapped_text(buf, "? \n",
						indent+1, 0, 0);

			otherbuf = parts_debug(part->parts, indent+3);
			strbuf_addbuf(buf, otherbuf);
			strbuf_release(otherbuf);
			free(otherbuf);
		}

		if (part->alt_parts) {
			strbuf_addstr(buf, "\n");
			strbuf_add_wrapped_text(buf, ": \n",
						indent+1, 0, 0);

			otherbuf = parts_debug(part->alt_parts, indent+3);
			strbuf_addbuf(buf, otherbuf);
			strbuf_release(otherbuf);
			free(otherbuf);
		}

		if (part->parts || part->alt_parts)
			strbuf_add_wrapped_text(buf, "}\n", indent, 0, 0);
		else
			strbuf_addstr(buf, "}\n");
	}
	strbuf_add_wrapped_text(buf, "}\n", --indent, 0, 0);

	if( !indent ){
		printf("%s", buf->buf);
		strbuf_release(buf);
		free(buf);
		return NULL;
	}
	return buf;
}

struct format_part *parse_extended(const char *unparsed)
{
	struct format_part *part = format_part_alloc();
	struct format_parse_state state = {0};
	const char *c = unparsed + 2;
	int condition = 0;
	c += strspn(c, " \t\r\n");

	if (*c == '"') {
		part->type = FORMAT_PART_FORMAT;
		state.expect_quote = 1;
		c++;
	} else if (!prefixcmp(c, "merge")) {
		part->type = FORMAT_PART_CONDITION_MERGE;
		condition = 1;
		c += 5;
	} else {
		part->type = FORMAT_PART_FORMAT;
		state.expect_paren = 1;
		state.ignore_space = 1;
	}

	if (condition) {
		c += strspn(c, " \t\r\n");
		if (*c != '?'){
			printf("NO-?\n");
			goto fail;
		}

		c++;
		c += strspn(c, " \t\r\n");

		if (*c == '"') {
			state.expect_quote = 1;
			c++;
		}
		state.expect_colon = !state.expect_quote;
		state.expect_paren = !state.expect_quote;
		state.ignore_space = !state.expect_quote;
	}

	printf("parse_format: %s\n", c);
	part->parts = parse_format(c, &state);
	if (!part->parts){
		printf("failed to parse %s\n", c);
		goto fail;
	}

	c += part->parts->format_len;
	if( state.expect_quote )
		c++;

	if (condition) {
		memset(&state, 0, sizeof(state));
		c += strspn(c, " \t\r\n");

		if (*c == ':'){
			c++;
			c += strspn(c, " \t\r\n");

			if (*c == '"') {
				state.expect_quote = 1;
				c++;
			}
			state.expect_colon = !state.expect_quote;
			state.expect_paren = !state.expect_quote;
			state.ignore_space = !state.expect_quote;

			part->alt_parts = parse_format(c, &state);
			if (!part->alt_parts)
				goto fail;

			c += part->alt_parts->format_len;

			if (*c == ':')
				goto fail;

			if (state.expect_quote) {
				if (*c != '"')
					goto fail;
				c++;
			}

			c += strspn(c, " \t\r\n");
		}
	} else {
		printf("not condition: %s\n", c);
		c += strspn(c, " \t\r\n");
	}

	if (*c != ')'){
		printf("no end in sight: %s\n", c);
		goto fail;
	}
	c++;

	part->format = xstrndup(unparsed, c - unparsed);
	part->format_len = c - unparsed;
	return part;

fail:
	printf("EXTENDED FAIL: %s -- %s\n", unparsed, c);
	format_part_free(&part);
	return NULL;
}

struct format_part *parse_special(const char *unparsed)
{
	struct format_part *part = format_part_alloc();
	switch (unparsed[1]) {
		case 'h':
			part->type = FORMAT_PART_COMMIT_HASH_ABBREV;
			part->format = xstrndup(unparsed, 2);
			part->format_len = strlen(part->format);
			return part;
		case 'H':
			part->type = FORMAT_PART_COMMIT_HASH;
			part->format = xstrndup(unparsed, 2);
			part->format_len = strlen(part->format);
			return part;
		case 'p':
			part->type = FORMAT_PART_PARENT_HASHES_ABBREV;
			part->format = xstrndup(unparsed, 2);
			part->format_len = strlen(part->format);
			return part;
		case 'P':
			part->type = FORMAT_PART_PARENT_HASHES;
			part->format = xstrndup(unparsed, 2);
			part->format_len = strlen(part->format);
			return part;
		case '%':
			part->type = FORMAT_PART_LITERAL;
			part->format = xstrndup(unparsed, 2);
			part->format_len = strlen(part->format);
			part->literal = "%";
			return part;
		case '(':
			return parse_extended(unparsed);
	}

	format_part_free(&part);
	return NULL;
}

struct format_parts *parse_format(const char *unparsed,
				  struct format_parse_state *state)
{
	struct format_parts *parts = format_parts_alloc();
	struct format_part *part;
	const char *c = unparsed;
	const char *last = NULL;
	char special[11];
	char dspecial[11];
	
	sprintf(special, "%%%s%s%s%s",
		state->expect_quote ? "\\\"" : "",
		state->expect_colon || state->expect_paren ? ":?" : "",
		state->expect_paren ? ")" : "",
		state->ignore_space ? " \t\r\n" : "");
	sprintf(dspecial, "%%%s%s%s%s",
		state->expect_quote ? "\\\"" : "",
		state->expect_colon || state->expect_paren ? ":?" : "",
		state->expect_paren ? ")" : "",
		state->ignore_space ? " trn" : "");
	printf("parsing: \"%s\" with specials: [%s]\n", unparsed, dspecial);

	while (*c) { 
		printf("CHAR: %c, last: %s\n", *c, last?last:"-");

		if (!last)
			last = c;

		c += strcspn(c, special);
		if (!*c)
			break;

		switch (*c) {
			case '%':
				part = parse_special(c);
				if (part) {
					parts_add_nliteral(parts, last, c - last);
					last = NULL;

					parts_add_part(parts, part);
					c += part->format_len;
					free(part);
					continue;
				}
				break;
			case ')':
				if (state->expect_paren) {
					state->found = ')';
					goto success;
				}
				break;
			case '?':
				if (state->expect_colon || state->expect_paren) {
					state->found = '?';
					goto fail;
				}
				break;
			case ':':
				if (state->expect_colon || state->expect_paren) {
					state->found = ':';		
					if (state->expect_colon)
						goto success;
					goto fail;
				}
				break;
			case '\\':
				if (state->expect_quote) {
					parts_add_nliteral(parts, last, c - last);
					last = NULL;
					parts_add_literal(parts, "\"");
					c += 1;
				}
				break;
			case '"':
				if (state->expect_quote) {
					state->found = '"';
					goto success;
				}
				break;
			case ' ':
			case '\t':
			case '\r':
			case '\n':
				if (state->ignore_space) {
					if (last)
						parts_add_nliteral(parts, last, c - last);
					last = NULL;
					c += strspn(c, " \t\r\n");
					continue;
				}
				break;
		}
		c++;
	}

	printf("[natural end]\n");

success:
	if (last)
		parts_add_nliteral(parts, last, c - last);

	parts->format = xstrndup(unparsed, c - unparsed);
	parts->format_len = c - unparsed;
	printf("END OF FORMAT: (%d) %*s\n", parts->format_len, parts->format_len, parts->format);
	return parts;

fail:
	format_parts_free(&parts);
	printf("ABORT FORMAT\n");
	return NULL;
}

// should never return NULL;
struct format_parts *parse(const char *unparsed)
{
	struct format_parse_state state = {0};
	return parse_format(unparsed, &state);
}

static int quiet = 0;

static struct option builtin_parse_format_options[] = {
	OPT__QUIET(&quiet),
	OPT_END()
};

int cmd_parse_format(int argc, const char **argv, const char *prefix)
{
	const char *unparsed;
	struct format_parts *parsed;

	argc = parse_options(argc, argv, prefix, builtin_parse_format_options,
			     builtin_parse_format_usage, 0);
	if (!argc)
		usage_with_options(builtin_parse_format_usage,
			           builtin_parse_format_options);

	unparsed = argv[0];
	printf("unparsed: \"%s\"\n", unparsed);

	parsed = parse( unparsed );

	if( !quiet )
		parts_debug(parsed, 0);
	return 0;
}
