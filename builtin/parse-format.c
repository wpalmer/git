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
notes:refspec
pretty:format-or-alias
format:format
tformat:format?

pretty: "hello"
%(pretty: git-maybe-mergeline)
		^-- if git-maybe-mergeline references itself, then literal
		-> but how do we determine this?
			-> pass in redirection-depth, and abort if > num_aliases?
			-> but that will only abort /one/, we want the alias to act
				as if undefined if it references itself, right?

foo = %(pretty:bar)
bar = %(pretty:baz)
baz = %(pretty:foo)

--pretty=%(pretty:foo)
	-> should treat %(pretty:foo) as literal, because "foo" is not a valid alias, because it references itself
		-> so, parse all aliases as we read them? seems wasteful, but simplifies things. This way alias
		   loops brought on by %(pretty:...) specification are caught at the same time other loops are.
		-> but it makes more sense to me to actually make the reference at the time of thingy


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
	FORMAT_PART_TREE_HASH,
	FORMAT_PART_TREE_HASH_ABBREV,

	FORMAT_PART_AUTHOR_NAME,
	FORMAT_PART_AUTHOR_NAME_MAILMAP,
	FORMAT_PART_AUTHOR_EMAIL,
	FORMAT_PART_AUTHOR_EMAIL_MAILMAP,
	FORMAT_PART_AUTHOR_DATE,
	FORMAT_PART_AUTHOR_DATE_RFC2822,
	FORMAT_PART_AUTHOR_DATE_RELATIVE,
	FORMAT_PART_AUTHOR_DATE_UNIX,
	FORMAT_PART_AUTHOR_DATE_ISO8601,
	FORMAT_PART_COMMITTER_NAME,
	FORMAT_PART_COMMITTER_NAME_MAILMAP,
	FORMAT_PART_COMMITTER_EMAIL,
	FORMAT_PART_COMMITTER_EMAIL_MAILMAP,
	FORMAT_PART_COMMITTER_DATE,
	FORMAT_PART_COMMITTER_DATE_RFC2822,
	FORMAT_PART_COMMITTER_DATE_RELATIVE,
	FORMAT_PART_COMMITTER_DATE_UNIX,
	FORMAT_PART_COMMITTER_DATE_ISO8601,

	FORMAT_PART_DECORATE,
	FORMAT_PART_ENCODING,
	FORMAT_PART_SUBJECT,
	FORMAT_PART_SUBJECT_SANITIZED,
	FORMAT_PART_BODY,
	FORMAT_PART_RAW_BODY,
	FORMAT_PART_NOTES,

	FORMAT_PART_REFLOG_SELECTOR,
	FORMAT_PART_REFLOG_SELECTOR_SHORT,
	FORMAT_PART_REFLOG_SUBJECT,

	FORMAT_PART_COLOR,
	FORMAT_PART_MARK,
	FORMAT_PART_WRAP,

	FORMAT_PART_CONDITION_MERGE
};

enum format_part_magic {
	NO_MAGIC,
	ADD_LF_BEFORE_NON_EMPTY,
	DEL_LF_BEFORE_EMPTY
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
	enum format_part_magic	magic;

	char			*format;
	size_t			format_len;

	size_t			literal_len;
	char			*literal;

	size_t			argc;
	char			**argv;

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
	if ((*part)->argv)
		free((*part)->argv);
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
			{FORMAT_PART_COMMIT_HASH, "COMMIT_HASH"},
			{FORMAT_PART_COMMIT_HASH_ABBREV, "COMMIT_HASH_ABBREV"},
			{FORMAT_PART_PARENT_HASHES, "PARENT_HASHES"},
			{FORMAT_PART_PARENT_HASHES_ABBREV, "PARENT_HASHES_ABBREV"},
			{FORMAT_PART_TREE_HASH, "TREE_HASH"},
			{FORMAT_PART_TREE_HASH_ABBREV, "TREE_HASH_ABBREV"},
			{FORMAT_PART_AUTHOR_NAME, "AUTHOR_NAME"},
			{FORMAT_PART_AUTHOR_NAME_MAILMAP, "AUTHOR_NAME_MAILMAP"},
			{FORMAT_PART_AUTHOR_EMAIL, "AUTHOR_EMAIL"},
			{FORMAT_PART_AUTHOR_EMAIL_MAILMAP, "AUTHOR_EMAIL_MAILMAP"},
			{FORMAT_PART_AUTHOR_DATE, "AUTHOR_DATE"},
			{FORMAT_PART_AUTHOR_DATE_RFC2822, "AUTHOR_DATE_RFC2822"},
			{FORMAT_PART_AUTHOR_DATE_RELATIVE, "AUTHOR_DATE_RELATIVE"},
			{FORMAT_PART_AUTHOR_DATE_UNIX, "AUTHOR_DATE_UNIX"},
			{FORMAT_PART_AUTHOR_DATE_ISO8601, "AUTHOR_DATE_ISO8601"},
			{FORMAT_PART_COMMITTER_NAME, "COMMITTER_NAME"},
			{FORMAT_PART_COMMITTER_NAME_MAILMAP, "COMMITTER_NAME_MAILMAP"},
			{FORMAT_PART_COMMITTER_EMAIL, "COMMITTER_EMAIL"},
			{FORMAT_PART_COMMITTER_EMAIL_MAILMAP, "COMMITTER_EMAIL_MAILMAP"},
			{FORMAT_PART_COMMITTER_DATE, "COMMITTER_DATE"},
			{FORMAT_PART_COMMITTER_DATE_RFC2822, "COMMITTER_DATE_RFC2822"},
			{FORMAT_PART_COMMITTER_DATE_RELATIVE, "COMMITTER_DATE_RELATIVE"},
			{FORMAT_PART_COMMITTER_DATE_UNIX, "COMMITTER_DATE_UNIX"},
			{FORMAT_PART_COMMITTER_DATE_ISO8601, "COMMITTER_DATE_ISO8601"},

			{FORMAT_PART_DECORATE, "DECORATE"},
			{FORMAT_PART_ENCODING, "ENCODING"},
			{FORMAT_PART_SUBJECT, "SUBJECT"},
			{FORMAT_PART_SUBJECT_SANITIZED, "SUBJECT_SANITIZED"},
			{FORMAT_PART_BODY, "BODY"},
			{FORMAT_PART_RAW_BODY, "RAW_BODY"},
			{FORMAT_PART_NOTES, "NOTES"},

			{FORMAT_PART_REFLOG_SELECTOR, "REFLOG_SELECTOR"},
			{FORMAT_PART_REFLOG_SELECTOR_SHORT, "REFLOG_SELECTOR_SHORT"},
			{FORMAT_PART_REFLOG_SUBJECT, "REFLOG_SUBJECT"},

			{FORMAT_PART_COLOR, "COLOR"},
			{FORMAT_PART_MARK, "MARK"},
			{FORMAT_PART_WRAP, "WRAP"},
			{FORMAT_PART_CONDITION_MERGE, "CONDITION:MERGE"}
		};
	char *label;
	size_t i,j;
	strbuf_init(buf, 0);

	strbuf_add_wrapped_text(buf, "{[PARTS:", indent++, 0, 0);
	strbuf_addf(buf, "%li]\n", parts->len);
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

		if (part->magic) {
			strbuf_addf(buf, " (%s)",
				    part->magic == ADD_LF_BEFORE_NON_EMPTY ?
				     "ADD_LF_BEFORE_NON_EMPTY" :
				     "DEL_LF_BEFORE_EMPTY");
		}

		if (part->literal) {
			strbuf_addstr(buf, " ");
			strbuf_add_wrapped_text(buf, part->literal,
						0, indent+strlen(label)+1, 0);
		}

		if (part->argc) {
			strbuf_addstr(buf, "\n");
			strbuf_add_wrapped_text(buf, "ARGS: [", indent+1, 0, 0);
			for (j = 0; j < part->argc; j++) {
				strbuf_add_wrapped_text(buf, part->argv[j],
							0, indent+8, 0);

				if (j < part->argc - 1)
					strbuf_addstr(buf, ", ");
			}
			strbuf_addstr(buf, "]\n");
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
			if (!part->alt_parts)
				strbuf_addstr(buf, "\n");
			strbuf_add_wrapped_text(buf, ": \n",
						indent+1, 0, 0);

			otherbuf = parts_debug(part->alt_parts, indent+3);
			strbuf_addbuf(buf, otherbuf);
			strbuf_release(otherbuf);
			free(otherbuf);
		}

		if (part->argc || part->parts || part->alt_parts)
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
		if (*c != '?')
			goto fail;

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

	part->parts = parse_format(c, &state);
	if (!part->parts)
		goto fail;

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
	} else
		c += strspn(c, " \t\r\n");

	if (*c != ')')
		goto fail;
	c++;

	part->format = xstrndup(unparsed, c - unparsed);
	part->format_len = c - unparsed;
	return part;

fail:
	format_part_free(&part);
	return NULL;
}

struct format_part *parse_special(const char *unparsed)
{
	struct format_part *part = format_part_alloc();
	int h1,h2;
	char c;
	const char *s, *e;

	switch (unparsed[1]) {
		case '-':
		case '+':
			if (*unparsed != '%')
				goto fail;

			format_part_free(&part);
			part = parse_special(unparsed + 1);
			if (part) {
				part->format_len++;
				free(part->format);
				part->format = xstrndup(unparsed, part->format_len);
				part->magic = unparsed[1] == '-' ?
					DEL_LF_BEFORE_EMPTY :
					ADD_LF_BEFORE_NON_EMPTY;
			}
			return part;
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
		case 't':
			part->type = FORMAT_PART_TREE_HASH_ABBREV;
			part->format = xstrndup(unparsed, 2);
			part->format_len = strlen(part->format);
			return part;
		case 'T':
			part->type = FORMAT_PART_TREE_HASH;
			part->format = xstrndup(unparsed, 2);
			part->format_len = strlen(part->format);
			return part;
		case 'a':
			switch (unparsed[2]) {
				case 'n':
					part->type = FORMAT_PART_AUTHOR_NAME;
					part->format = xstrndup(unparsed, 3);
					part->format_len = strlen(part->format);
					return part;
				case 'N':
					part->type = FORMAT_PART_AUTHOR_NAME_MAILMAP;
					part->format = xstrndup(unparsed, 3);
					part->format_len = strlen(part->format);
					return part;
				case 'e':
					part->type = FORMAT_PART_AUTHOR_EMAIL;
					part->format = xstrndup(unparsed, 3);
					part->format_len = strlen(part->format);
					return part;
				case 'E':
					part->type = FORMAT_PART_AUTHOR_EMAIL_MAILMAP;
					part->format = xstrndup(unparsed, 3);
					part->format_len = strlen(part->format);
					return part;
				case 'd':
					part->type = FORMAT_PART_AUTHOR_DATE;
					part->format = xstrndup(unparsed, 3);
					part->format_len = strlen(part->format);
					return part;
				case 'D':
					part->type = FORMAT_PART_AUTHOR_DATE_RFC2822;
					part->format = xstrndup(unparsed, 3);
					part->format_len = strlen(part->format);
					return part;
				case 'r':
					part->type = FORMAT_PART_AUTHOR_DATE_RELATIVE;
					part->format = xstrndup(unparsed, 3);
					part->format_len = strlen(part->format);
					return part;
				case 't':
					part->type = FORMAT_PART_AUTHOR_DATE_UNIX;
					part->format = xstrndup(unparsed, 3);
					part->format_len = strlen(part->format);
					return part;
				case 'i':
					part->type = FORMAT_PART_AUTHOR_DATE_ISO8601;
					part->format = xstrndup(unparsed, 3);
					part->format_len = strlen(part->format);
					return part;
					
			}
			break;
		case 'c':
			switch (unparsed[2]) {
				case 'n':
					part->type = FORMAT_PART_COMMITTER_NAME;
					part->format = xstrndup(unparsed, 3);
					part->format_len = strlen(part->format);
					return part;
				case 'N':
					part->type = FORMAT_PART_COMMITTER_NAME_MAILMAP;
					part->format = xstrndup(unparsed, 3);
					part->format_len = strlen(part->format);
					return part;
				case 'e':
					part->type = FORMAT_PART_COMMITTER_EMAIL;
					part->format = xstrndup(unparsed, 3);
					part->format_len = strlen(part->format);
					return part;
				case 'E':
					part->type = FORMAT_PART_COMMITTER_EMAIL_MAILMAP;
					part->format = xstrndup(unparsed, 3);
					part->format_len = strlen(part->format);
					return part;
				case 'd':
					part->type = FORMAT_PART_COMMITTER_DATE;
					part->format = xstrndup(unparsed, 3);
					part->format_len = strlen(part->format);
					return part;
				case 'D':
					part->type = FORMAT_PART_COMMITTER_DATE_RFC2822;
					part->format = xstrndup(unparsed, 3);
					part->format_len = strlen(part->format);
					return part;
				case 'r':
					part->type = FORMAT_PART_COMMITTER_DATE_RELATIVE;
					part->format = xstrndup(unparsed, 3);
					part->format_len = strlen(part->format);
					return part;
				case 't':
					part->type = FORMAT_PART_COMMITTER_DATE_UNIX;
					part->format = xstrndup(unparsed, 3);
					part->format_len = strlen(part->format);
					return part;
				case 'i':
					part->type = FORMAT_PART_COMMITTER_DATE_ISO8601;
					part->format = xstrndup(unparsed, 3);
					part->format_len = strlen(part->format);
					return part;
			}
			break;
		case 'd':
			part->type = FORMAT_PART_DECORATE;
			part->format = xstrndup(unparsed, 2);
			part->format_len = strlen(part->format);
			return part;
		case 'e':
			part->type = FORMAT_PART_ENCODING;
			part->format = xstrndup(unparsed, 2);
			part->format_len = strlen(part->format);
			return part;
		case 's':
			part->type = FORMAT_PART_SUBJECT;
			part->format = xstrndup(unparsed, 2);
			part->format_len = strlen(part->format);
			return part;
		case 'f':
			part->type = FORMAT_PART_SUBJECT_SANITIZED;
			part->format = xstrndup(unparsed, 2);
			part->format_len = strlen(part->format);
			return part;
		case 'b':
			part->type = FORMAT_PART_BODY;
			part->format = xstrndup(unparsed, 2);
			part->format_len = strlen(part->format);
			return part;
		case 'B':
			part->type = FORMAT_PART_RAW_BODY;
			part->format = xstrndup(unparsed, 2);
			part->format_len = strlen(part->format);
			return part;
		case 'N':
			part->type = FORMAT_PART_NOTES;
			part->format = xstrndup(unparsed, 2);
			part->format_len = strlen(part->format);
			return part;
		case 'g':
			switch (unparsed[2]) {
				case 'D':
					part->type = FORMAT_PART_REFLOG_SELECTOR;
					part->format = xstrndup(unparsed, 3);
					part->format_len = strlen(part->format);
					return part;
				case 'd':
					part->type = FORMAT_PART_REFLOG_SELECTOR_SHORT;
					part->format = xstrndup(unparsed, 3);
					part->format_len = strlen(part->format);
					return part;
				case 's':
					part->type = FORMAT_PART_REFLOG_SUBJECT;
					part->format = xstrndup(unparsed, 3);
					part->format_len = strlen(part->format);
					return part;
			}
			break;
		case 'C':
			part->type = FORMAT_PART_COLOR;
			part->argc = 1;
			part->argv = calloc(1, sizeof(char*));
			if (unparsed[2] == '(') {
				s = &unparsed[3];
				e = strchr(s, ')');
				if (e) {
					part->argv[0] = xstrndup(s, e - s);
					part->format = strndup(unparsed, e - unparsed + 1);
					part->format_len = strlen(part->format);
					return part;
				}
				break;
			}

			if (!prefixcmp(&unparsed[2], "red"))
				part->argv[0] = "red";
			else if (!prefixcmp(&unparsed[2], "green"))
				part->argv[0] = "green";
			else if (!prefixcmp(&unparsed[2], "blue"))
				part->argv[0] = "blue";
			else if (!prefixcmp(&unparsed[2], "reset"))
				part->argv[0] = "reset";

			if (part->argv[0]) {
				part->format = xstrndup(unparsed, 2+strlen(part->argv[0]));
				part->format_len = strlen(part->format);
				return part;
			}
			break;
		case 'm':
			part->type = FORMAT_PART_MARK;
			part->format = xstrndup(unparsed, 2);
			part->format_len = strlen(part->format);
			return part;
		case 'w':
			if (unparsed[2] != '(')
				break;

			part->type = FORMAT_PART_WRAP;

			s = unparsed + 3;
			s += strspn(s, " \t\r\n");
			if (*s == ')') {
				s++;
				part->format = xstrndup(unparsed, s - unparsed);
				part->format_len = strlen(part->format);
				return part;
			}

			while (part->argc < 3) {
				s += strspn(s, " \t\r\n");

				if (strcspn(s, "0123456789"))
					goto fail;

				e = s + strspn(s, "0123456789");
				if (e == s)
					goto fail;

				part->argv = xrealloc(part->argv,
						      sizeof(char*) * part->argc+1);
				part->argv[ part->argc ] = xstrndup(s, e - s);
				part->argc++;

				s = e + strspn(e, " \t\r\n");
				if (*s == ')') {
					s++;
					part->format = xstrndup(unparsed, s - unparsed);
					part->format_len = strlen(part->format);
					return part;
				}

				if (*s == ',')
					s++;
			}
			break;
		case 'x':
			/* %x00 == NUL, %x0a == LF, etc. */
			if (0 <= (h1 = hexval_table[0xff & unparsed[2]]) &&
			    h1 <= 16 &&
			    0 <= (h2 = hexval_table[0xff & unparsed[3]]) &&
			    h2 <= 16) {
				part->type = FORMAT_PART_LITERAL;
				part->format = xstrndup(unparsed, 4);
				part->format_len = strlen(part->format);
				c = (h1<<4)|h2;
				part->literal = xstrndup(&c,1);
				return part;
			}
			break;
		case 'n':
			part->type = FORMAT_PART_LITERAL;
			part->format = xstrndup(unparsed, 2);
			part->format_len = strlen(part->format);
			part->literal = "\n";
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

fail:
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
	
	sprintf(special, "%%%s%s%s%s",
		state->expect_quote ? "\\\"" : "",
		state->expect_colon || state->expect_paren ? ":?" : "",
		state->expect_paren ? ")" : "",
		state->ignore_space ? " \t\r\n" : "");

	while (*c) { 
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

success:
	if (last)
		parts_add_nliteral(parts, last, c - last);

	parts->format = xstrndup(unparsed, c - unparsed);
	parts->format_len = c - unparsed;
	return parts;

fail:
	format_parts_free(&parts);
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

	parsed = parse( unparsed );

	if( !quiet )
		parts_debug(parsed, 0);
	return 0;
}
