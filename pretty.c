#include "cache.h"
#include "commit.h"
#include "utf8.h"
#include "diff.h"
#include "revision.h"
#include "string-list.h"
#include "mailmap.h"
#include "log-tree.h"
#include "notes.h"
#include "color.h"
#include "reflog-walk.h"

static struct format_parts *user_format;
static struct cmt_fmt_map {
	const char *name;
	enum cmit_fmt format;
	int is_tformat;
	int is_alias;
	const char *user_format;
} *commit_formats;
static size_t builtin_formats_len;
static size_t commit_formats_len;
static size_t commit_formats_alloc;
static struct cmt_fmt_map *find_commit_format(const char *sought);

#define WHITESPACE " \t\r\n"
#define format_parts_alloc() \
	((struct format_parts*)xcalloc(1, sizeof(struct format_parts)))
#define format_part_alloc() \
	((struct format_part*)xcalloc(1, sizeof(struct format_part)))
static void format_part_free(struct format_part **part);
static void format_parts_free(struct format_parts **parts)
{
	if((*parts)->part)
		free((*parts)->part);
	free(*parts);
	*parts = NULL;
}
static void format_part_free(struct format_part **part)
{
	if ((*part)->literal)
		free((*part)->literal);
	if ((*part)->args)
		free((*part)->args);
	free(*part);
	*part = NULL;
}

static struct format_part *parts_add(struct format_parts *parts,
				     enum format_part_type type)
{
	ALLOC_GROW(parts->part, parts->len+1, parts->alloc);
	memset(&parts->part[parts->len], 0,
	       sizeof(parts->part[parts->len]));
	parts->part[parts->len].type = type;
	parts->len++;
	return &parts->part[parts->len-1];
}

static struct format_part *parts_add_part(struct format_parts *parts,
					   struct format_part *part)
{
	struct format_part *dst = parts_add(parts, FORMAT_PART_UNKNOWN);
	memcpy(dst, part, sizeof(*dst));
	if (part->type == FORMAT_PART_NOTES)
		parts->want.notes = 1;
	return dst;
}

static void parts_add_nliteral(struct format_parts *parts, const char *literal,
			       size_t len)
{
	if (len == 0)
		return;
	parts_add(parts, FORMAT_PART_LITERAL);
	parts->part[parts->len-1].literal = xmemdupz(literal, len);
	parts->part[parts->len-1].literal_len = len;
	parts->part[parts->len-1].format_len = len;
	return;
}

static void part_add_arg_date_mode(struct format_part *part,
				   enum date_mode dmode)
{
	part->args = xrealloc(part->args,
			      sizeof(struct format_arg) * (part->argc+1));
	part->args[part->argc].type = FORMAT_ARG_DATE_MODE;
	part->args[part->argc].dmode = dmode;
	part->argc++;
	return;
}

static void part_add_arg_boolean(struct format_part *part, int value)
{
	part->args = xrealloc(part->args,
			      sizeof(struct format_arg) * (part->argc+1));
	part->args[part->argc].type = FORMAT_ARG_BOOLEAN;
	part->args[part->argc].boolean = value ? 1 : 0;
	part->argc++;
	return;
}

/*
* Parse a single argument of an extended format, up to the next delimiter
* ie: up to ',' or ')'
* The return value is the position of the found delimiter within *unparsed,
* or NULL if the argument was invalid.
*/
const char *parse_arg(struct format_part *part, enum format_arg_type type,
		      const char *unparsed)
{
	struct format_arg arg = {0};
	const char *c = unparsed;
	char *t;
	size_t len;
	char date_format[DATE_FORMAT_MAX];

	arg.type = type;

	c += strspn(c, WHITESPACE);

	switch (type){
	case FORMAT_ARG_UINT:
		if (isdigit(*c)) {
			arg.uint = strtoul(c, &t, 10);
			c = t;
		}
		break;
	case FORMAT_ARG_DATE_MODE:
		len = strcspn(c, WHITESPACE ",)");
		if (len >= DATE_FORMAT_MAX)
			return NULL;
		strncpy(date_format, c, len);
		len = parse_date_format_len(date_format, &arg.dmode);
		if (!len)
			return NULL;
		c += len;
		break;
	default:
		return NULL;
	}

	c += strspn(c, WHITESPACE);
	if (*c == ',' || *c == ')'){
		ALLOC_GROW(part->args, part->argc+1, part->args_alloc);
		memcpy(&part->args[part->argc], &arg,
		       sizeof(struct format_arg));
		part->argc++;
		return c;
	}
	return NULL;
}

static struct format_part *parse_extended(const char *unparsed)
{
	struct format_part *part = format_part_alloc();
	const char *c = unparsed + 2; /* "%(..." + strlen("%(") */
	const char *e;

	c += strspn(c, WHITESPACE);

	if (!prefixcmp(c, "author") || !prefixcmp(c, "committer")) {
		e = c;
		c += (*e == 'a') ? 6 : 9;
		if (!prefixcmp(c, "date")) {
			part->type = (*e == 'a') ? FORMAT_PART_AUTHOR_DATE :
						   FORMAT_PART_COMMITTER_DATE;
			c += 4 + strspn(c + 4, WHITESPACE);
			if (*c == ')')
				goto success;
			if (*c != ':')
				goto fail;
			c = parse_arg(part, FORMAT_ARG_DATE_MODE, c+1);
			if (!c)
				goto fail;
			goto success;
		}
		if (!prefixcmp(c, "name") || !prefixcmp(c, "email")) {
			if (*c == 'n') { /* name */
				part->type = (*e == 'a') ? FORMAT_PART_AUTHOR_NAME :
							   FORMAT_PART_COMMITTER_NAME;
				c += 4;
			} else { /* email */
				part->type = (*e == 'a') ? FORMAT_PART_AUTHOR_EMAIL :
							   FORMAT_PART_COMMITTER_EMAIL;
				c += 5;
			}

			strspn(c, WHITESPACE);
			if (*c == ')')
				goto success;
			if (*c != ':')
				goto fail;
			c += 1 + strspn(c + 1, WHITESPACE);
			if (!prefixcmp(c, "mailmap")) {
				part_add_arg_boolean(part, 1);
				c += 7 + strspn(c + 7, WHITESPACE);
				if (*c == ')')
					goto success;
			}
			goto fail;
		}

		c = e;
	}

	if (!prefixcmp(c, "color")) {
		part->type = FORMAT_PART_LITERAL;
		c += 5 + strspn(c + 5, WHITESPACE);
		if (*c == ')') {
			part->literal = xstrdup(GIT_COLOR_RESET);
			part->literal_len = strlen(part->literal);
			goto success;
		}
		if (*c != ':')
			goto fail;
		c++;
		e = strchr(c, ')');
		part->literal = xcalloc(1, COLOR_MAXLEN);
		if (!e || !color_parse_len(c, e - c,
					   part->literal))
			goto fail;
		part->literal_len = strlen(part->literal);
		c = e;
		goto success;
	}

	if (!prefixcmp(c, "wrap")) {
		part->type = FORMAT_PART_WRAP;
		c += 4;
		while(part->argc <= 3){
			c += strspn(c, WHITESPACE);
			if (*c == ')')
				goto success;
			if (*c != (part->argc ? ',' : ':'))
				goto fail;
			if (part->argc == 3)
				goto fail;

			c = parse_arg(part, FORMAT_ARG_UINT, c+1);
			if (!c)
				goto fail;
		}
		goto fail;
	}

fail:
	format_part_free(&part);
	return NULL;

success:
	part->format_len = c - unparsed + 1;
	return part;
}

static struct format_part *parse_special(const char *unparsed)
{
	struct format_part *part = NULL;
	int h1,h2;
	char c;
	const char *s, *e;

	/* these allocate their own part */
	switch (unparsed[1]) {
	case '-':
	case '+':
	case ' ':
		if (*unparsed != '%')
			goto fail;

		part = parse_special(unparsed + 1);
		if (part) {
			part->format_len++;

			switch (unparsed[1]) {
			case '-':
				part->magic = DEL_LF_BEFORE_EMPTY;
				break;
			case '+':
				part->magic = ADD_LF_BEFORE_NON_EMPTY;
				break;
			case ' ':
				part->magic = ADD_SP_BEFORE_NON_EMPTY;
				break;
			}
		}
		return part;
	case '(':
		return parse_extended(unparsed);
	}

	part = format_part_alloc();

	/* most placeholders are 2 characters long */
	part->format_len = 2;

	switch (unparsed[1]) {
	case 'h':
		part->type = FORMAT_PART_COMMIT_HASH_ABBREV;
		return part;
	case 'H':
		part->type = FORMAT_PART_COMMIT_HASH;
		return part;
	case 'p':
		part->type = FORMAT_PART_PARENT_HASHES_ABBREV;
		return part;
	case 'P':
		part->type = FORMAT_PART_PARENT_HASHES;
		return part;
	case 't':
		part->type = FORMAT_PART_TREE_HASH_ABBREV;
		return part;
	case 'T':
		part->type = FORMAT_PART_TREE_HASH;
		return part;
	case 'a':
		part->format_len++;
		switch (unparsed[2]) {
		case 'n':
			part->type = FORMAT_PART_AUTHOR_NAME;
			return part;
		case 'N':
			part->type = FORMAT_PART_AUTHOR_NAME;
			part_add_arg_boolean(part, 1);
			return part;
		case 'e':
			part->type = FORMAT_PART_AUTHOR_EMAIL;
			return part;
		case 'E':
			part->type = FORMAT_PART_AUTHOR_EMAIL;
			part_add_arg_boolean(part, 1);
			return part;
		case 'd':
			part->type = FORMAT_PART_AUTHOR_DATE;
			return part;
		case 'D':
			part->type = FORMAT_PART_AUTHOR_DATE;
			part_add_arg_date_mode(part, DATE_RFC2822);
			return part;
		case 'r':
			part->type = FORMAT_PART_AUTHOR_DATE;
			part_add_arg_date_mode(part, DATE_RELATIVE);
			return part;
		case 't':
			part->type = FORMAT_PART_AUTHOR_DATE;
			part_add_arg_date_mode(part, DATE_UNIX);
			return part;
		case 'i':
			part->type = FORMAT_PART_AUTHOR_DATE;
			part_add_arg_date_mode(part, DATE_ISO8601);
			return part;
		}
		goto fail;
	case 'c':
		part->format_len++;
		switch (unparsed[2]) {
		case 'n':
			part->type = FORMAT_PART_COMMITTER_NAME;
			return part;
		case 'N':
			part->type = FORMAT_PART_COMMITTER_NAME;
			part_add_arg_boolean(part, 1);
			return part;
		case 'e':
			part->type = FORMAT_PART_COMMITTER_EMAIL;
			return part;
		case 'E':
			part->type = FORMAT_PART_COMMITTER_EMAIL;
			part_add_arg_boolean(part, 1);
			return part;
		case 'd':
			part->type = FORMAT_PART_COMMITTER_DATE;
			return part;
		case 'D':
			part->type = FORMAT_PART_COMMITTER_DATE;
			part_add_arg_date_mode(part, DATE_RFC2822);
			return part;
		case 'r':
			part->type = FORMAT_PART_COMMITTER_DATE;
			part_add_arg_date_mode(part, DATE_RELATIVE);
			return part;
		case 't':
			part->type = FORMAT_PART_COMMITTER_DATE;
			part_add_arg_date_mode(part, DATE_UNIX);
			return part;
		case 'i':
			part->type = FORMAT_PART_COMMITTER_DATE;
			part_add_arg_date_mode(part, DATE_ISO8601);
			return part;
		}
		goto fail;
	case 'd':
		part->type = FORMAT_PART_DECORATE;
		return part;
	case 'e':
		part->type = FORMAT_PART_ENCODING;
		return part;
	case 's':
		part->type = FORMAT_PART_SUBJECT;
		return part;
	case 'f':
		part->type = FORMAT_PART_SUBJECT_SANITIZED;
		return part;
	case 'b':
		part->type = FORMAT_PART_BODY;
		return part;
	case 'B':
		part->type = FORMAT_PART_RAW_BODY;
		return part;
	case 'N':
		part->type = FORMAT_PART_NOTES;
		return part;
	case 'g':
		part->format_len++;
		switch (unparsed[2]) {
		case 'D':
			part->type = FORMAT_PART_REFLOG_SELECTOR;
			return part;
		case 'd':
			part->type = FORMAT_PART_REFLOG_SELECTOR_SHORT;
			return part;
		case 's':
			part->type = FORMAT_PART_REFLOG_SUBJECT;
			return part;
		}
		goto fail;
	case 'C':
		part->type = FORMAT_PART_LITERAL;
		if (unparsed[2] == '(') {
			e = strchr(unparsed + 3, ')');
			part->literal = xcalloc(1, COLOR_MAXLEN);
			if (!e || !color_parse_len(unparsed + 3,
						  e - (unparsed + 3),
						  part->literal))
				goto fail;
			part->literal_len = strlen(part->literal);
			part->format_len = e - unparsed + 1;
			return part;
		}

		if (!prefixcmp(&unparsed[2], "red")) {
			part->literal = GIT_COLOR_RED;
			part->literal_len = strlen(GIT_COLOR_RED);
			part->format_len = 5;
		} else if (!prefixcmp(&unparsed[2], "green")) {
			part->literal = GIT_COLOR_GREEN;
			part->literal_len = strlen(GIT_COLOR_GREEN);
			part->format_len = 7;
		} else if (!prefixcmp(&unparsed[2], "blue")) {
			part->literal = GIT_COLOR_BLUE;
			part->literal_len = strlen(GIT_COLOR_BLUE);
			part->format_len = 6;
		} else if (!prefixcmp(&unparsed[2], "reset")) {
			part->literal = GIT_COLOR_RESET;
			part->literal_len = strlen(GIT_COLOR_RESET);
			part->format_len = 7;
		}

		if (part->literal)
			return part;
		goto fail;
	case 'm':
		part->type = FORMAT_PART_MARK;
		return part;
	case 'w':
		if (unparsed[2] != '(')
			goto fail;

		part->type = FORMAT_PART_WRAP;

		s = unparsed + 3;
		while (part->argc <= 3) {
			s += strspn(s, WHITESPACE);
			if (*s == ')'){
				part->format_len = s - unparsed + 1;
				return part;
			}
			if (part->argc) {
				if (*s != ',')
					goto fail;
				s++;
			}
			if (part->argc == 3)
				goto fail;

			s = parse_arg(part, FORMAT_ARG_UINT, s);
			if (!s)
				goto fail;
		}
		goto fail;
	case 'x':
		/* %x00 == NUL, %x0a == LF, etc. */
		if (0 <= (h1 = hexval_table[0xff & unparsed[2]]) &&
		    h1 <= 16 &&
		    0 <= (h2 = hexval_table[0xff & unparsed[3]]) &&
		    h2 <= 16) {
			part->type = FORMAT_PART_LITERAL;
			part->format_len = 4;
			c = (h1<<4)|h2;
			part->literal = xmemdupz(&c, 1);
			part->literal_len = 1;
			return part;
		}
		goto fail;
	case 'n':
		part->type = FORMAT_PART_LITERAL;
		part->literal = "\n";
		part->literal_len = 1;
		return part;
	case '%':
		part->type = FORMAT_PART_LITERAL;
		part->literal = xstrndup(&unparsed[1], 1);
		part->literal_len = 1;
		return part;
	}

fail:
	if (part)
		format_part_free(&part);
	return NULL;
}

struct format_parts *parse_format(const char *unparsed)
{
	struct format_parts *parts = format_parts_alloc();
	struct format_part *part;
	const char *c = unparsed;
	const char *last = NULL;

	while (*c) {
		if (!last)
			last = c;

		c += strcspn(c, "%");
		if (!*c)
			break;

		part = parse_special(c);
		if (part) {
			parts_add_nliteral(parts, last, c - last);
			last = NULL;

			parts_add_part(parts, part);
			c += part->format_len;
			free(part);
			continue;
		}
		c++;
	}

	if (last)
		parts_add_nliteral(parts, last, c - last);

	parts->format_len = c - unparsed + 1;
	return parts;
}

static void save_user_format(struct rev_info *rev, const char *cp, int is_tformat)
{
	if (user_format)
		format_parts_free(&user_format);
	user_format = parse_format(cp);
	if (is_tformat)
		rev->use_terminator = 1;
	rev->commit_format = CMIT_FMT_USERFORMAT;
}

static int git_pretty_formats_config(const char *var, const char *value, void *cb)
{
	struct cmt_fmt_map *commit_format = NULL;
	const char *name;
	const char *fmt;
	int i;

	if (prefixcmp(var, "pretty."))
		return 0;

	name = var + strlen("pretty.");
	for (i = 0; i < builtin_formats_len; i++) {
		if (!strcmp(commit_formats[i].name, name))
			return 0;
	}

	for (i = builtin_formats_len; i < commit_formats_len; i++) {
		if (!strcmp(commit_formats[i].name, name)) {
			commit_format = &commit_formats[i];
			break;
		}
	}

	if (!commit_format) {
		ALLOC_GROW(commit_formats, commit_formats_len+1,
			   commit_formats_alloc);
		commit_format = &commit_formats[commit_formats_len];
		memset(commit_format, 0, sizeof(*commit_format));
		commit_formats_len++;
	}

	commit_format->name = xstrdup(name);
	commit_format->format = CMIT_FMT_USERFORMAT;
	git_config_string(&fmt, var, value);
	if (!prefixcmp(fmt, "format:") || !prefixcmp(fmt, "tformat:")) {
		commit_format->is_tformat = fmt[0] == 't';
		fmt = strchr(fmt, ':') + 1;
	} else if (strchr(fmt, '%'))
		commit_format->is_tformat = 1;
	else
		commit_format->is_alias = 1;
	commit_format->user_format = fmt;

	return 0;
}

static void setup_commit_formats(void)
{
	struct cmt_fmt_map builtin_formats[] = {
		{ "raw",	CMIT_FMT_RAW,		0 },
		{ "medium",	CMIT_FMT_MEDIUM,	0 },
		{ "short",	CMIT_FMT_SHORT,		0 },
		{ "email",	CMIT_FMT_EMAIL,		0 },
		{ "fuller",	CMIT_FMT_FULLER,	0 },
		{ "full",	CMIT_FMT_FULL,		0 },
		{ "oneline",	CMIT_FMT_ONELINE,	1 }
	};
	commit_formats_len = ARRAY_SIZE(builtin_formats);
	builtin_formats_len = commit_formats_len;
	ALLOC_GROW(commit_formats, commit_formats_len, commit_formats_alloc);
	memcpy(commit_formats, builtin_formats,
	       sizeof(*builtin_formats)*ARRAY_SIZE(builtin_formats));

	git_config(git_pretty_formats_config, NULL);
}

static struct cmt_fmt_map *find_commit_format_recursive(const char *sought,
							const char *original,
							int num_redirections)
{
	struct cmt_fmt_map *found = NULL;
	size_t found_match_len = 0;
	int i;

	if (num_redirections >= commit_formats_len)
		die("invalid --pretty format: "
		    "'%s' references an alias which points to itself",
		    original);

	for (i = 0; i < commit_formats_len; i++) {
		size_t match_len;

		if (prefixcmp(commit_formats[i].name, sought))
			continue;

		match_len = strlen(commit_formats[i].name);
		if (found == NULL || found_match_len > match_len) {
			found = &commit_formats[i];
			found_match_len = match_len;
		}
	}

	if (found && found->is_alias) {
		found = find_commit_format_recursive(found->user_format,
						     original,
						     num_redirections+1);
	}

	return found;
}

static struct cmt_fmt_map *find_commit_format(const char *sought)
{
	if (!commit_formats)
		setup_commit_formats();

	return find_commit_format_recursive(sought, sought, 0);
}

void get_commit_format(const char *arg, struct rev_info *rev)
{
	struct cmt_fmt_map *commit_format;

	rev->use_terminator = 0;
	if (!arg || !*arg) {
		rev->commit_format = CMIT_FMT_DEFAULT;
		return;
	}
	if (!prefixcmp(arg, "format:") || !prefixcmp(arg, "tformat:")) {
		save_user_format(rev, strchr(arg, ':') + 1, arg[0] == 't');
		return;
	}

	if (strchr(arg, '%')) {
		save_user_format(rev, arg, 1);
		return;
	}

	commit_format = find_commit_format(arg);
	if (!commit_format)
		die("invalid --pretty format: %s", arg);

	rev->commit_format = commit_format->format;
	rev->use_terminator = commit_format->is_tformat;
	if (commit_format->format == CMIT_FMT_USERFORMAT) {
		save_user_format(rev, commit_format->user_format,
				 commit_format->is_tformat);
	}
}

/*
 * Generic support for pretty-printing the header
 */
static int get_one_line(const char *msg)
{
	int ret = 0;

	for (;;) {
		char c = *msg++;
		if (!c)
			break;
		ret++;
		if (c == '\n')
			break;
	}
	return ret;
}

/* High bit set, or ISO-2022-INT */
static int non_ascii(int ch)
{
	return !isascii(ch) || ch == '\033';
}

int has_non_ascii(const char *s)
{
	int ch;
	if (!s)
		return 0;
	while ((ch = *s++) != '\0') {
		if (non_ascii(ch))
			return 1;
	}
	return 0;
}

static int is_rfc2047_special(char ch)
{
	return (non_ascii(ch) || (ch == '=') || (ch == '?') || (ch == '_'));
}

static void add_rfc2047(struct strbuf *sb, const char *line, int len,
		       const char *encoding)
{
	int i, last;

	for (i = 0; i < len; i++) {
		int ch = line[i];
		if (non_ascii(ch))
			goto needquote;
		if ((i + 1 < len) && (ch == '=' && line[i+1] == '?'))
			goto needquote;
	}
	strbuf_add(sb, line, len);
	return;

needquote:
	strbuf_grow(sb, len * 3 + strlen(encoding) + 100);
	strbuf_addf(sb, "=?%s?q?", encoding);
	for (i = last = 0; i < len; i++) {
		unsigned ch = line[i] & 0xFF;
		/*
		 * We encode ' ' using '=20' even though rfc2047
		 * allows using '_' for readability.  Unfortunately,
		 * many programs do not understand this and just
		 * leave the underscore in place.
		 */
		if (is_rfc2047_special(ch) || ch == ' ') {
			strbuf_add(sb, line + last, i - last);
			strbuf_addf(sb, "=%02X", ch);
			last = i + 1;
		}
	}
	strbuf_add(sb, line + last, len - last);
	strbuf_addstr(sb, "?=");
}

void pp_user_info(const char *what, enum cmit_fmt fmt, struct strbuf *sb,
		  const char *line, enum date_mode dmode,
		  const char *encoding)
{
	char *date;
	int namelen;
	unsigned long time;
	int tz;

	if (fmt == CMIT_FMT_ONELINE)
		return;
	date = strchr(line, '>');
	if (!date)
		return;
	namelen = ++date - line;
	time = strtoul(date, &date, 10);
	tz = strtol(date, NULL, 10);

	if (fmt == CMIT_FMT_EMAIL) {
		char *name_tail = strchr(line, '<');
		int display_name_length;
		if (!name_tail)
			return;
		while (line < name_tail && isspace(name_tail[-1]))
			name_tail--;
		display_name_length = name_tail - line;
		strbuf_addstr(sb, "From: ");
		add_rfc2047(sb, line, display_name_length, encoding);
		strbuf_add(sb, name_tail, namelen - display_name_length);
		strbuf_addch(sb, '\n');
	} else {
		strbuf_addf(sb, "%s: %.*s%.*s\n", what,
			      (fmt == CMIT_FMT_FULLER) ? 4 : 0,
			      "    ", namelen, line);
	}
	switch (fmt) {
	case CMIT_FMT_MEDIUM:
		strbuf_addf(sb, "Date:   %s\n", show_date(time, tz, dmode));
		break;
	case CMIT_FMT_EMAIL:
		strbuf_addf(sb, "Date: %s\n", show_date(time, tz, DATE_RFC2822));
		break;
	case CMIT_FMT_FULLER:
		strbuf_addf(sb, "%sDate: %s\n", what, show_date(time, tz, dmode));
		break;
	default:
		/* notin' */
		break;
	}
}

static int is_empty_line(const char *line, int *len_p)
{
	int len = *len_p;
	while (len && isspace(line[len-1]))
		len--;
	*len_p = len;
	return !len;
}

static const char *skip_empty_lines(const char *msg)
{
	for (;;) {
		int linelen = get_one_line(msg);
		int ll = linelen;
		if (!linelen)
			break;
		if (!is_empty_line(msg, &ll))
			break;
		msg += linelen;
	}
	return msg;
}

static void add_merge_info(enum cmit_fmt fmt, struct strbuf *sb,
			const struct commit *commit, int abbrev)
{
	struct commit_list *parent = commit->parents;

	if ((fmt == CMIT_FMT_ONELINE) || (fmt == CMIT_FMT_EMAIL) ||
	    !parent || !parent->next)
		return;

	strbuf_addstr(sb, "Merge:");

	while (parent) {
		struct commit *p = parent->item;
		const char *hex = NULL;
		if (abbrev)
			hex = find_unique_abbrev(p->object.sha1, abbrev);
		if (!hex)
			hex = sha1_to_hex(p->object.sha1);
		parent = parent->next;

		strbuf_addf(sb, " %s", hex);
	}
	strbuf_addch(sb, '\n');
}

static char *get_header(const struct commit *commit, const char *key)
{
	int key_len = strlen(key);
	const char *line = commit->buffer;

	for (;;) {
		const char *eol = strchr(line, '\n'), *next;

		if (line == eol)
			return NULL;
		if (!eol) {
			eol = line + strlen(line);
			next = NULL;
		} else
			next = eol + 1;
		if (eol - line > key_len &&
		    !strncmp(line, key, key_len) &&
		    line[key_len] == ' ') {
			return xmemdupz(line + key_len + 1, eol - line - key_len - 1);
		}
		line = next;
	}
}

static char *replace_encoding_header(char *buf, const char *encoding)
{
	struct strbuf tmp = STRBUF_INIT;
	size_t start, len;
	char *cp = buf;

	/* guess if there is an encoding header before a \n\n */
	while (strncmp(cp, "encoding ", strlen("encoding "))) {
		cp = strchr(cp, '\n');
		if (!cp || *++cp == '\n')
			return buf;
	}
	start = cp - buf;
	cp = strchr(cp, '\n');
	if (!cp)
		return buf; /* should not happen but be defensive */
	len = cp + 1 - (buf + start);

	strbuf_attach(&tmp, buf, strlen(buf), strlen(buf) + 1);
	if (is_encoding_utf8(encoding)) {
		/* we have re-coded to UTF-8; drop the header */
		strbuf_remove(&tmp, start, len);
	} else {
		/* just replaces XXXX in 'encoding XXXX\n' */
		strbuf_splice(&tmp, start + strlen("encoding "),
					  len - strlen("encoding \n"),
					  encoding, strlen(encoding));
	}
	return strbuf_detach(&tmp, NULL);
}

char *logmsg_reencode(const struct commit *commit,
		      const char *output_encoding)
{
	static const char *utf8 = "UTF-8";
	const char *use_encoding;
	char *encoding;
	char *out;

	if (!*output_encoding)
		return NULL;
	encoding = get_header(commit, "encoding");
	use_encoding = encoding ? encoding : utf8;
	if (!strcmp(use_encoding, output_encoding))
		if (encoding) /* we'll strip encoding header later */
			out = xstrdup(commit->buffer);
		else
			return NULL; /* nothing to do */
	else
		out = reencode_string(commit->buffer,
				      output_encoding, use_encoding);
	if (out)
		out = replace_encoding_header(out, output_encoding);

	free(encoding);
	return out;
}

static int mailmap_name(char *email, int email_len, char *name, int name_len)
{
	static struct string_list *mail_map;
	if (!mail_map) {
		mail_map = xcalloc(1, sizeof(*mail_map));
		read_mailmap(mail_map, NULL);
	}
	return mail_map->nr && map_user(mail_map, email, email_len, name, name_len);
}

static void format_person_part(struct strbuf *sb, struct format_part *part,
			       const char *msg, int len, enum date_mode dmode)
{
	int start, end, tz = 0;
	unsigned long date = 0;
	char *ep;
	const char *name_start, *name_end, *mail_start, *mail_end, *msg_end = msg+len;
	size_t name_len, mail_len;
	char person_name[1024];
	char person_mail[1024];

	/* advance 'end' to point to email start delimiter */
	for (end = 0; end < len && msg[end] != '<'; end++)
		; /* do nothing */

	/*
	 * When end points at the '<' that we found, it should have
	 * matching '>' later, which means 'end' must be strictly
	 * below len - 1.
	 */
	if (end >= len - 2)
		return;

	/* Seek for both name and email part */
	name_start = msg;
	name_end = msg+end;
	while (name_end > name_start && isspace(*(name_end-1)))
		name_end--;
	name_len = name_end-name_start;
	if (name_len >= sizeof(person_name))
		return;
	mail_start = msg+end+1;
	mail_end = mail_start;
	while (mail_end < msg_end && *mail_end != '>')
		mail_end++;
	mail_len = mail_end-mail_start;
	if (mail_len >= sizeof(person_mail))
		return;
	if (mail_end == msg_end)
		return;
	end = mail_end-msg;

	if ((part->type == FORMAT_PART_AUTHOR_NAME ||
	     part->type == FORMAT_PART_AUTHOR_EMAIL ||
	     part->type == FORMAT_PART_COMMITTER_NAME ||
	     part->type == FORMAT_PART_COMMITTER_EMAIL) &&
	    part->argc && part->args[0].boolean) { /* mailmap */
		/* copy up to, and including, the end delimiter */
		strlcpy(person_name, name_start, name_len+1);
		strlcpy(person_mail, mail_start, mail_len+1);
		mailmap_name(person_mail, sizeof(person_mail), person_name, sizeof(person_name));
		name_start = person_name;
		name_len = strlen(person_name);
		mail_start = person_mail;
		mail_len = strlen(person_mail);
	}
	if (part->type == FORMAT_PART_AUTHOR_NAME ||
	    part->type == FORMAT_PART_COMMITTER_NAME) {
		strbuf_add(sb, name_start, name_len);
		return;
	}
	if (part->type == FORMAT_PART_AUTHOR_EMAIL ||
	    part->type == FORMAT_PART_COMMITTER_EMAIL) {
		strbuf_add(sb, mail_start, mail_len);
		return;
	}

	/* advance 'start' to point to date start delimiter */
	for (start = end + 1; start < len && isspace(msg[start]); start++)
		; /* do nothing */
	if (start >= len)
		return;
	date = strtoul(msg + start, &ep, 10);
	if (msg + start == ep)
		return;

	if (part->type != FORMAT_PART_AUTHOR_DATE &&
	    part->type != FORMAT_PART_COMMITTER_DATE)
		return;

	if (part->argc && part->args[0].dmode == DATE_UNIX) {
		strbuf_add(sb, msg + start, ep - (msg + start));
		return;
	}

	/* parse tz */
	for (start = ep - msg + 1; start < len && isspace(msg[start]); start++)
		; /* do nothing */
	if (start + 1 < len) {
		tz = strtoul(msg + start + 1, NULL, 10);
		if (msg[start] == '-')
			tz = -tz;
	}

	if (part->argc)
		strbuf_addstr(sb, show_date(date, tz, part->args[0].dmode));
	else
		strbuf_addstr(sb, show_date(date, tz, dmode));
	return;
}

struct chunk {
	size_t off;
	size_t len;
};

struct format_commit_context {
	const struct commit *commit;
	const struct pretty_print_context *pretty_ctx;
	unsigned commit_header_parsed:1;
	unsigned commit_message_parsed:1;
	char *message;
	size_t width, indent1, indent2;

	/* These offsets are relative to the start of the commit message. */
	struct chunk author;
	struct chunk committer;
	struct chunk encoding;
	size_t message_off;
	size_t subject_off;
	size_t body_off;

	/* The following ones are relative to the result struct strbuf. */
	struct chunk abbrev_commit_hash;
	struct chunk abbrev_tree_hash;
	struct chunk abbrev_parent_hashes;
	size_t wrap_start;
};

static int add_again(struct strbuf *sb, struct chunk *chunk)
{
	if (chunk->len) {
		strbuf_adddup(sb, chunk->off, chunk->len);
		return 1;
	}

	/*
	 * We haven't seen this chunk before.  Our caller is surely
	 * going to add it the hard way now.  Remember the most likely
	 * start of the to-be-added chunk: the current end of the
	 * struct strbuf.
	 */
	chunk->off = sb->len;
	return 0;
}

static void parse_commit_header(struct format_commit_context *context)
{
	const char *msg = context->message;
	int i;

	for (i = 0; msg[i]; i++) {
		int eol;
		for (eol = i; msg[eol] && msg[eol] != '\n'; eol++)
			; /* do nothing */

		if (i == eol) {
			break;
		} else if (!prefixcmp(msg + i, "author ")) {
			context->author.off = i + 7;
			context->author.len = eol - i - 7;
		} else if (!prefixcmp(msg + i, "committer ")) {
			context->committer.off = i + 10;
			context->committer.len = eol - i - 10;
		} else if (!prefixcmp(msg + i, "encoding ")) {
			context->encoding.off = i + 9;
			context->encoding.len = eol - i - 9;
		}
		i = eol;
	}
	context->message_off = i;
	context->commit_header_parsed = 1;
}

static int istitlechar(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		(c >= '0' && c <= '9') || c == '.' || c == '_';
}

static void format_sanitized_subject(struct strbuf *sb, const char *msg)
{
	size_t trimlen;
	size_t start_len = sb->len;
	int space = 2;

	for (; *msg && *msg != '\n'; msg++) {
		if (istitlechar(*msg)) {
			if (space == 1)
				strbuf_addch(sb, '-');
			space = 0;
			strbuf_addch(sb, *msg);
			if (*msg == '.')
				while (*(msg+1) == '.')
					msg++;
		} else
			space |= 1;
	}

	/* trim any trailing '.' or '-' characters */
	trimlen = 0;
	while (sb->len - trimlen > start_len &&
		(sb->buf[sb->len - 1 - trimlen] == '.'
		|| sb->buf[sb->len - 1 - trimlen] == '-'))
		trimlen++;
	strbuf_remove(sb, sb->len - trimlen, trimlen);
}

const char *format_subject(struct strbuf *sb, const char *msg,
			   const char *line_separator)
{
	int first = 1;

	for (;;) {
		const char *line = msg;
		int linelen = get_one_line(line);

		msg += linelen;
		if (!linelen || is_empty_line(line, &linelen))
			break;

		if (!sb)
			continue;
		strbuf_grow(sb, linelen + 2);
		if (!first)
			strbuf_addstr(sb, line_separator);
		strbuf_add(sb, line, linelen);
		first = 0;
	}
	return msg;
}

static void parse_commit_message(struct format_commit_context *c)
{
	const char *msg = c->message + c->message_off;
	const char *start = c->message;

	msg = skip_empty_lines(msg);
	c->subject_off = msg - start;

	msg = format_subject(NULL, msg, NULL);
	msg = skip_empty_lines(msg);
	c->body_off = msg - start;

	c->commit_message_parsed = 1;
}

static void format_decoration(struct strbuf *sb, const struct commit *commit)
{
	struct name_decoration *d;
	const char *prefix = " (";

	load_ref_decorations(DECORATE_SHORT_REFS);
	d = lookup_decoration(&name_decoration, &commit->object);
	while (d) {
		strbuf_addstr(sb, prefix);
		prefix = ", ";
		strbuf_addstr(sb, d->name);
		d = d->next;
	}
	if (prefix[0] == ',')
		strbuf_addch(sb, ')');
}

static void strbuf_wrap(struct strbuf *sb, size_t pos,
			size_t width, size_t indent1, size_t indent2)
{
	struct strbuf tmp = STRBUF_INIT;

	if (pos)
		strbuf_add(&tmp, sb->buf, pos);
	strbuf_add_wrapped_text(&tmp, sb->buf + pos,
				(int) indent1, (int) indent2, (int) width);
	strbuf_swap(&tmp, sb);
	strbuf_release(&tmp);
}

static void rewrap_message_tail(struct strbuf *sb,
				struct format_commit_context *c,
				size_t new_width, size_t new_indent1,
				size_t new_indent2)
{
	if (c->width == new_width && c->indent1 == new_indent1 &&
	    c->indent2 == new_indent2)
		return;
	if (c->wrap_start < sb->len)
		strbuf_wrap(sb, c->wrap_start, c->width, c->indent1, c->indent2);
	c->wrap_start = sb->len;
	c->width = new_width;
	c->indent1 = new_indent1;
	c->indent2 = new_indent2;
}

void format_commit_message_part(struct format_part *part,
				struct strbuf *sb, void *context)
{
	struct format_commit_context *c = context;
	const struct commit *commit = c->commit;
	const char *msg = commit->buffer;
	struct commit_list *p;
	unsigned long width = 0, indent1 = 0, indent2 = 0;

	/* these are independent of the commit */
	switch (part->type) {
	case FORMAT_PART_LITERAL:
		strbuf_add(sb, part->literal, part->literal_len);
		return;
	case FORMAT_PART_WRAP:
		width = (part->argc > 0) ? part->args[0].uint : 0;
		indent1 = (part->argc > 1) ? part->args[1].uint : 0;
		indent2 = (part->argc > 2) ? part->args[2].uint : 0;
		rewrap_message_tail(sb, c, width, indent1, indent2);
		return;
	default:
		break;
	}

	/* these depend on the commit */
	if (!commit->object.parsed)
		parse_object(commit->object.sha1);

	switch (part->type) {
	case FORMAT_PART_COMMIT_HASH:
		strbuf_addstr(sb, sha1_to_hex(commit->object.sha1));
		return;
	case FORMAT_PART_COMMIT_HASH_ABBREV:
		if (add_again(sb, &c->abbrev_commit_hash))
			return;
		strbuf_addstr(sb, find_unique_abbrev(commit->object.sha1,
					     c->pretty_ctx->abbrev));
		c->abbrev_commit_hash.len = sb->len - c->abbrev_commit_hash.off;
		return;
	case FORMAT_PART_TREE_HASH:
		strbuf_addstr(sb, sha1_to_hex(commit->tree->object.sha1));
		return;
	case FORMAT_PART_TREE_HASH_ABBREV:
		if (add_again(sb, &c->abbrev_tree_hash))
			return;
		strbuf_addstr(sb, find_unique_abbrev(commit->tree->object.sha1,
						     c->pretty_ctx->abbrev));
		c->abbrev_tree_hash.len = sb->len - c->abbrev_tree_hash.off;
		return;
	case FORMAT_PART_PARENT_HASHES:
		for (p = commit->parents; p; p = p->next) {
			if (p != commit->parents)
				strbuf_addch(sb, ' ');
			strbuf_addstr(sb, sha1_to_hex(p->item->object.sha1));
		}
		return;
	case FORMAT_PART_PARENT_HASHES_ABBREV:
		if (add_again(sb, &c->abbrev_parent_hashes))
			return;
		for (p = commit->parents; p; p = p->next) {
			if (p != commit->parents)
				strbuf_addch(sb, ' ');
			strbuf_addstr(sb, find_unique_abbrev(
					p->item->object.sha1,
					c->pretty_ctx->abbrev));
		}
		c->abbrev_parent_hashes.len = sb->len -
		                              c->abbrev_parent_hashes.off;
		return;
	case FORMAT_PART_MARK:
		strbuf_addch(sb, (commit->object.flags & BOUNDARY)
		                 ? '-'
		                 : (commit->object.flags & SYMMETRIC_LEFT)
		                 ? '<'
		                 : '>');
		return;
	case FORMAT_PART_DECORATE:
		format_decoration(sb, commit);
		return;
	case FORMAT_PART_REFLOG_SELECTOR:
	case FORMAT_PART_REFLOG_SELECTOR_SHORT:
		if (c->pretty_ctx->reflog_info) {
			get_reflog_selector(sb,
					    c->pretty_ctx->reflog_info,
					    c->pretty_ctx->date_mode,
					    (part->type == FORMAT_PART_REFLOG_SELECTOR_SHORT));
		}
		return;
	case FORMAT_PART_REFLOG_SUBJECT:
		if (c->pretty_ctx->reflog_info)
			get_reflog_message(sb, c->pretty_ctx->reflog_info);
		return;
	case FORMAT_PART_NOTES:
		if (c->pretty_ctx->show_notes) {
			format_display_notes(commit->object.sha1, sb,
				    get_log_output_encoding(), 0);
		}
		return;
	default:
		break;
	}

	/* For the rest we have to parse the commit header. */
	if (!c->commit_header_parsed)
		parse_commit_header(c);

	switch (part->type) {
	case FORMAT_PART_AUTHOR_NAME:
	case FORMAT_PART_AUTHOR_EMAIL:
	case FORMAT_PART_AUTHOR_DATE:
		format_person_part(sb, part, commit->buffer + c->author.off,
				   c->author.len, c->pretty_ctx->date_mode);
		return;
	case FORMAT_PART_COMMITTER_NAME:
	case FORMAT_PART_COMMITTER_EMAIL:
	case FORMAT_PART_COMMITTER_DATE:
		format_person_part(sb, part, commit->buffer + c->committer.off,
				   c->committer.len, c->pretty_ctx->date_mode);
		return;
	case FORMAT_PART_ENCODING:
		strbuf_add(sb, msg + c->encoding.off, c->encoding.len);
		return;
	case FORMAT_PART_RAW_BODY:
		/* message_off is always left at the initial newline */
		strbuf_addstr(sb, msg + c->message_off + 1);
		return;
	default:
		break;
	}

	/* Now we need to parse the commit message. */
	if (!c->commit_message_parsed)
		parse_commit_message(c);

	switch (part->type) {
	case FORMAT_PART_SUBJECT:
		format_subject(sb, msg + c->subject_off, " ");
		return;
	case FORMAT_PART_SUBJECT_SANITIZED:
		format_sanitized_subject(sb, msg + c->subject_off);
		return;
	case FORMAT_PART_BODY:
		strbuf_addstr(sb, msg + c->body_off);
		return;
	default:
		break;
	}
	return;
}

void format_commit_message_parts(const struct format_parts *parsed,
				 struct strbuf *sb, void *context)
{
	size_t i, orig_len;
	enum format_part_magic magic;

	for (i = 0; i < parsed->len; i++) {
		orig_len = sb->len;
		magic = parsed->part[i].magic;
		format_commit_message_part(&parsed->part[i], sb, context);

		if (magic == NO_MAGIC)
			continue;

		if ((orig_len == sb->len) && magic == DEL_LF_BEFORE_EMPTY) {
			while (sb->len && sb->buf[sb->len - 1] == '\n')
				strbuf_setlen(sb, sb->len - 1);
		} else if (orig_len != sb->len) {
			if (magic == ADD_LF_BEFORE_NON_EMPTY)
				strbuf_insert(sb, orig_len, "\n", 1);
			else if (magic == ADD_SP_BEFORE_NON_EMPTY)
				strbuf_insert(sb, orig_len, " ", 1);
		}
	}
}

void userformat_find_requirements(const char *fmt, struct userformat_want *w)
{
	struct format_parts *dummy;

	if (!fmt) {
		if (!user_format)
			return;
		memcpy(w, &user_format->want, sizeof(*w));
		return;
	}

	dummy = parse_format(fmt);
	memcpy(w, &dummy->want, sizeof(*w));
	format_parts_free(&dummy);
}

void format_commit_message_parsed(const struct commit *commit,
				  const struct format_parts *parsed_format,
				  struct strbuf *sb,
				  const struct pretty_print_context *pretty_ctx)
{
	struct format_commit_context context;
	static const char utf8[] = "UTF-8";
	const char *enc;
	const char *output_enc = pretty_ctx->output_encoding;

	memset(&context, 0, sizeof(context));
	context.commit = commit;
	context.pretty_ctx = pretty_ctx;
	context.wrap_start = sb->len;
	context.message = commit->buffer;
	if (output_enc) {
		enc = get_header(commit, "encoding");
		enc = enc ? enc : utf8;
		if (strcmp(enc, output_enc))
			context.message = logmsg_reencode(commit, output_enc);
	}

	format_commit_message_parts(parsed_format, sb, &context);
	rewrap_message_tail(sb, &context, 0, 0, 0);

	if (context.message != commit->buffer)
		free(context.message);
}

void format_commit_message(const struct commit *commit,
			   const char *format, struct strbuf *sb,
			   const struct pretty_print_context *pretty_ctx)
{
	static char *last = NULL;
	static struct format_parts *parsed = NULL;

	if( !parsed || strcmp(last, format) ){
		if (parsed){
			format_parts_free(&parsed);
			free(last);
		}
		parsed = parse_format(format);
		last = xstrdup(format);
	}

	format_commit_message_parsed(commit, parsed, sb, pretty_ctx);
}

static void pp_header(enum cmit_fmt fmt,
		      int abbrev,
		      enum date_mode dmode,
		      const char *encoding,
		      const struct commit *commit,
		      const char **msg_p,
		      struct strbuf *sb)
{
	int parents_shown = 0;

	for (;;) {
		const char *line = *msg_p;
		int linelen = get_one_line(*msg_p);

		if (!linelen)
			return;
		*msg_p += linelen;

		if (linelen == 1)
			/* End of header */
			return;

		if (fmt == CMIT_FMT_RAW) {
			strbuf_add(sb, line, linelen);
			continue;
		}

		if (!memcmp(line, "parent ", 7)) {
			if (linelen != 48)
				die("bad parent line in commit");
			continue;
		}

		if (!parents_shown) {
			struct commit_list *parent;
			int num;
			for (parent = commit->parents, num = 0;
			     parent;
			     parent = parent->next, num++)
				;
			/* with enough slop */
			strbuf_grow(sb, num * 50 + 20);
			add_merge_info(fmt, sb, commit, abbrev);
			parents_shown = 1;
		}

		/*
		 * MEDIUM == DEFAULT shows only author with dates.
		 * FULL shows both authors but not dates.
		 * FULLER shows both authors and dates.
		 */
		if (!memcmp(line, "author ", 7)) {
			strbuf_grow(sb, linelen + 80);
			pp_user_info("Author", fmt, sb, line + 7, dmode, encoding);
		}
		if (!memcmp(line, "committer ", 10) &&
		    (fmt == CMIT_FMT_FULL || fmt == CMIT_FMT_FULLER)) {
			strbuf_grow(sb, linelen + 80);
			pp_user_info("Commit", fmt, sb, line + 10, dmode, encoding);
		}
	}
}

void pp_title_line(enum cmit_fmt fmt,
		   const char **msg_p,
		   struct strbuf *sb,
		   const char *subject,
		   const char *after_subject,
		   const char *encoding,
		   int need_8bit_cte)
{
	const char *line_separator = (fmt == CMIT_FMT_EMAIL) ? "\n " : " ";
	struct strbuf title;

	strbuf_init(&title, 80);
	*msg_p = format_subject(&title, *msg_p, line_separator);

	strbuf_grow(sb, title.len + 1024);
	if (subject) {
		strbuf_addstr(sb, subject);
		add_rfc2047(sb, title.buf, title.len, encoding);
	} else {
		strbuf_addbuf(sb, &title);
	}
	strbuf_addch(sb, '\n');

	if (need_8bit_cte > 0) {
		const char *header_fmt =
			"MIME-Version: 1.0\n"
			"Content-Type: text/plain; charset=%s\n"
			"Content-Transfer-Encoding: 8bit\n";
		strbuf_addf(sb, header_fmt, encoding);
	}
	if (after_subject) {
		strbuf_addstr(sb, after_subject);
	}
	if (fmt == CMIT_FMT_EMAIL) {
		strbuf_addch(sb, '\n');
	}
	strbuf_release(&title);
}

void pp_remainder(enum cmit_fmt fmt,
		  const char **msg_p,
		  struct strbuf *sb,
		  int indent)
{
	int first = 1;
	for (;;) {
		const char *line = *msg_p;
		int linelen = get_one_line(line);
		*msg_p += linelen;

		if (!linelen)
			break;

		if (is_empty_line(line, &linelen)) {
			if (first)
				continue;
			if (fmt == CMIT_FMT_SHORT)
				break;
		}
		first = 0;

		strbuf_grow(sb, linelen + indent + 20);
		if (indent) {
			memset(sb->buf + sb->len, ' ', indent);
			strbuf_setlen(sb, sb->len + indent);
		}
		strbuf_add(sb, line, linelen);
		strbuf_addch(sb, '\n');
	}
}

char *reencode_commit_message(const struct commit *commit, const char **encoding_p)
{
	const char *encoding;

	encoding = get_log_output_encoding();
	if (encoding_p)
		*encoding_p = encoding;
	return logmsg_reencode(commit, encoding);
}

void pretty_print_commit(enum cmit_fmt fmt, const struct commit *commit,
			 struct strbuf *sb,
			 const struct pretty_print_context *context)
{
	unsigned long beginning_of_body;
	int indent = 4;
	const char *msg = commit->buffer;
	char *reencoded;
	const char *encoding;
	int need_8bit_cte = context->need_8bit_cte;

	if (fmt == CMIT_FMT_USERFORMAT) {
		format_commit_message_parsed(commit, user_format, sb, context);
		return;
	}

	reencoded = reencode_commit_message(commit, &encoding);
	if (reencoded) {
		msg = reencoded;
	}

	if (fmt == CMIT_FMT_ONELINE || fmt == CMIT_FMT_EMAIL)
		indent = 0;

	/*
	 * We need to check and emit Content-type: to mark it
	 * as 8-bit if we haven't done so.
	 */
	if (fmt == CMIT_FMT_EMAIL && need_8bit_cte == 0) {
		int i, ch, in_body;

		for (in_body = i = 0; (ch = msg[i]); i++) {
			if (!in_body) {
				/* author could be non 7-bit ASCII but
				 * the log may be so; skip over the
				 * header part first.
				 */
				if (ch == '\n' && msg[i+1] == '\n')
					in_body = 1;
			}
			else if (non_ascii(ch)) {
				need_8bit_cte = 1;
				break;
			}
		}
	}

	pp_header(fmt, context->abbrev, context->date_mode, encoding,
		  commit, &msg, sb);
	if (fmt != CMIT_FMT_ONELINE && !context->subject) {
		strbuf_addch(sb, '\n');
	}

	/* Skip excess blank lines at the beginning of body, if any... */
	msg = skip_empty_lines(msg);

	/* These formats treat the title line specially. */
	if (fmt == CMIT_FMT_ONELINE || fmt == CMIT_FMT_EMAIL)
		pp_title_line(fmt, &msg, sb, context->subject,
			      context->after_subject, encoding, need_8bit_cte);

	beginning_of_body = sb->len;
	if (fmt != CMIT_FMT_ONELINE)
		pp_remainder(fmt, &msg, sb, indent);
	strbuf_rtrim(sb);

	/* Make sure there is an EOLN for the non-oneline case */
	if (fmt != CMIT_FMT_ONELINE)
		strbuf_addch(sb, '\n');

	/*
	 * The caller may append additional body text in e-mail
	 * format.  Make sure we did not strip the blank line
	 * between the header and the body.
	 */
	if (fmt == CMIT_FMT_EMAIL && sb->len <= beginning_of_body)
		strbuf_addch(sb, '\n');

	if (context->show_notes)
		format_display_notes(commit->object.sha1, sb, encoding,
				     NOTES_SHOW_HEADER | NOTES_INDENT);

	free(reencoded);
}
