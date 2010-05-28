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

*/

static const char * const builtin_parse_format_usage[] = {
	"git parse-format <format-string>",
	NULL
};

enum format_part_type {
	FORMAT_PART_UNKNOWN,
	FORMAT_PART_LITERAL,
	FORMAT_PART_COMMIT_HASH,
	FORMAT_PART_COMMIT_HASH_ABBREV,
	FORMAT_PART_PARENT_HASHES,
	FORMAT_PART_PARENT_HASHES_ABBREV
};

struct format_part;
struct format_parts {
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

#define format_parts_alloc() \
	((struct format_parts*)calloc(1, sizeof(struct format_parts)))
#define format_part_alloc() \
	((struct format_part*)calloc(1, sizeof(struct format_part)))
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

static void parts_debug(struct format_parts *parts, size_t indent)
{
	struct format_part *part;
	struct strbuf buf = {0};
	size_t i;
	strbuf_init(&buf, 0);

	strbuf_add_wrapped_text(&buf, "{[PARTS:", indent++, 0, 0);
	strbuf_addf(&buf, "%d]\n", parts->len);
	for (i = 0; i < parts->len; i++) {
		part = &parts->part[i];
		switch(part->type){
			case FORMAT_PART_LITERAL:
				strbuf_add_wrapped_text(&buf, "{LITERAL ",
							indent, indent, 0);
				strbuf_add_wrapped_text(&buf, part->literal,
							0, indent+9, 0);
				strbuf_add(&buf, "}\n", 2);
				break;
			case FORMAT_PART_COMMIT_HASH:
				strbuf_add_wrapped_text(&buf, "{COMMIT_HASH}\n",
							indent, 0, 0);
				break;
			case FORMAT_PART_COMMIT_HASH_ABBREV:
				strbuf_add_wrapped_text(&buf,
							"{COMMIT_HASH_ABBREV}"
							"\n",
							indent, 0, 0);
				break;
			case FORMAT_PART_PARENT_HASHES:
				strbuf_add_wrapped_text(&buf,
							"{PARENT_HASHES}\n",
							indent, 0, 0);
				break;
			case FORMAT_PART_PARENT_HASHES_ABBREV:
				strbuf_add_wrapped_text(&buf,
							"{PARENT_HASHES_ABBREV}"
							"\n",
							indent, 0, 0);
				break;
			default:
				strbuf_add_wrapped_text(&buf, "{UNKNOWN}\n",
							indent, 0, 0);
				break;
		}
	}
	strbuf_add_wrapped_text(&buf, "}\n", --indent, 0, 0);

	if( !indent ){
		printf("%s", buf.buf);
	}
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
	}
	return NULL;
}

struct format_parts *parse(const char *unparsed)
{
	struct format_parts *parts = format_parts_alloc();
	struct format_part *part;
	const char *c = unparsed;
	const char *last = NULL;
	printf("parsing: \"%s\"\n", unparsed);

	while (*c) { 
		printf("CHAR: %c, last: %s\n", *c, last?last:"-");

		if (!last)
			last = c;

		c += strcspn(c, "%)?:\"");
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

				//if next character is %, literal %.
				//else, parse_special
				//which should return a length and fill a parse_part
				// maybe just return a parse_part, which should keep track of
				// the length of its origin format
				break;
			case ')':
				//if expecting ), return end-of-paren
				//else, literal )

				/*
				but it's not as simple as that:
					- we may be inside parens, but not expecting them yet
						%(a-condition)
						             ^-- HERE, we were expecting space or ?
					- we may be inside parens, and parens are okay, but we're
					  also inside an explicit literal
				*/
				break;
			case '?':
				break;
			case ':':
				break;
			case '"':
				break;
			default:
				break;
		}
		c++;
	}

	if (last)
		parts_add_nliteral(parts, last, c - last);

	printf("END OF FORMAT\n");
	return parts;
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
