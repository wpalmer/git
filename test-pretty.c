#include <ctype.h>
#include "cache.h"
#include "utf8.h"
#include "commit.h"

static const char *usage_msg = "\n"
"  test-pretty <format>\n"
"  test-pretty -a\n"
"  test-pretty -- <format>\n";

static const char *all = "a"
"%-h%+h% h"
"%h%H%p%P%t%T"
"%an%aN%ae%aE%ad%aD%ar%at%ai"
"%cn%cN%ce%cE%cd%cD%cr%ct%ci"
"%d%e%s%f%b%B%N"
"%gD%gd%gs"
"%Cred%Cgreen%Cblue%Creset%C(reset)"
"%m%w()%w(1)%w(1,2)%w(1,2,3)"
"%x0a%n%%%@";

static struct strbuf *parts_debug(struct format_parts *parts,
				  const char *unparsed)
{
	struct format_part *part;
	struct strbuf *buf = xcalloc(1, sizeof(*buf));
	size_t indent = 0;
	struct {enum format_part_type type; char *label;} labels[] = {
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
		{FORMAT_PART_COMMITTER_NAME, "COMMITTER_NAME"},
		{FORMAT_PART_COMMITTER_NAME_MAILMAP, "COMMITTER_NAME_MAILMAP"},
		{FORMAT_PART_COMMITTER_EMAIL, "COMMITTER_EMAIL"},
		{FORMAT_PART_COMMITTER_EMAIL_MAILMAP, "COMMITTER_EMAIL_MAILMAP"},
		{FORMAT_PART_COMMITTER_DATE, "COMMITTER_DATE"},

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

		{FORMAT_PART_MARK, "MARK"},
		{FORMAT_PART_WRAP, "WRAP"}
	};
	char *label;
	size_t i,j,t = 0;
	strbuf_init(buf, 0);

	strbuf_add_wrapped_text(buf, "{[PARTS:", indent++, 0, 0);
	strbuf_addf(buf, "%li]\n", parts->len);
	strbuf_add_wrapped_text(buf, "[FORMAT:", indent, 0, 0);
	strbuf_addf(buf, "%s]\n", unparsed);
	strbuf_add_wrapped_text(buf, "(REMADE:", indent, 0, 0);
	for (i = 0; i < parts->len; i++) {
		strbuf_add(buf, unparsed + t, parts->part[i].format_len);
		t += parts->part[i].format_len;
	}
	strbuf_addstr(buf, ")\n");

	for (i = 0; i < parts->len; i++) {
		part = &parts->part[i];
		label = "UNKNOWN";
		for (j = 0; j < ARRAY_SIZE(labels); j++) {
			if (labels[j].type == part->type) {
				label = labels[j].label;
			}
		}

		strbuf_add_wrapped_text(buf, "{[", indent, 0, 0);
		strbuf_add(buf, unparsed, part->format_len);
		unparsed += part->format_len;
		strbuf_add(buf, "] ", 2);
		strbuf_addstr(buf, label);

		switch(part->magic){
		case NO_MAGIC:
			break;
		case ADD_LF_BEFORE_NON_EMPTY:
			strbuf_addstr(buf, " (ADD_LF_BEFORE_NON_EMPTY)");
			break;
		case DEL_LF_BEFORE_EMPTY:
			strbuf_addstr(buf, " (DEL_LF_BEFORE_EMPTY)");
			break;
		case ADD_SP_BEFORE_NON_EMPTY:
			strbuf_addstr(buf, " (ADD_SP_BEFORE_NON_EMPTY)");
			break;
		}

		if (part->literal) {
			strbuf_addstr(buf, " \"");
			t = 0;
			while (t < part->literal_len) {
				switch (part->literal[t]) {
				case '\n':
					strbuf_addstr(buf, "\\n");
					break;
				case '\r':
					strbuf_addstr(buf, "\\r");
					break;
				case '\t':
					strbuf_addstr(buf, "\\t");
					break;
				case '\\':
					strbuf_addstr(buf, "\\\\");
					break;
				default:
					if (isprint(part->literal[t]))
						strbuf_add(buf, &part->literal[t],
							   1);
					else
						strbuf_addf(buf, "\\x%02x",
							    part->literal[t]);
					break;
				}
				t++;
			}
			strbuf_addstr(buf, "\"");
		}

		if (part->argc) {
			strbuf_addstr(buf, "\n");
			strbuf_add_wrapped_text(buf, "ARGS: [", indent+1, 0, 0);
			for (j = 0; j < part->argc; j++) {
				switch(part->args[j].type){
				case FORMAT_ARG_UINT:
					strbuf_addstr(buf, "UINT:");
					strbuf_addf(buf, "%lu", part->args[j].uint);
					break;
				case FORMAT_ARG_DATE_MODE:
					strbuf_addstr(buf, "DATE_MODE:");
					switch(part->args[j].dmode){
					case DATE_NORMAL:
						strbuf_addstr(buf, "DATE_NORMAL");
						break;
					case DATE_RELATIVE:
						strbuf_addstr(buf, "DATE_RELATIVE");
						break;
					case DATE_SHORT:
						strbuf_addstr(buf, "DATE_SHORT");
						break;
					case DATE_LOCAL:
						strbuf_addstr(buf, "DATE_LOCAL");
						break;
					case DATE_ISO8601:
						strbuf_addstr(buf, "DATE_ISO8601");
						break;
					case DATE_RFC2822:
						strbuf_addstr(buf, "DATE_RFC2822");
						break;
					case DATE_RAW:
						strbuf_addstr(buf, "DATE_RAW");
						break;
					case DATE_UNIX:
						strbuf_addstr(buf, "DATE_UNIX");
						break;
					default:
						strbuf_addf(buf, "(UNKNOWN:%u)",
							    part->args[j].dmode);
						break;
					}
					break;
				default:
					strbuf_addstr(buf, "(UNKNOWN)");
					break;
				}

				if (j < part->argc - 1)
					strbuf_addstr(buf, ", ");
			}
			strbuf_addstr(buf, "]\n");
		}

		if (part->argc)
			strbuf_add_wrapped_text(buf, "}\n", indent, 0, 0);
		else
			strbuf_addstr(buf, "}\n");
	}
	strbuf_add_wrapped_text(buf, "}\n", --indent, 0, 0);

	if (!indent) {
		printf("%s", buf->buf);
		strbuf_release(buf);
		free(buf);
		return NULL;
	}
	return buf;
}

int main(int argc, char **argv)
{
	const char *unparsed = NULL;
	struct format_parts *parsed;

	if (argc < 2) {
		usage(usage_msg);
		return 1;
	}

	if (*argv[1] == '-') {
		if (argv[1][1] == 'a' && argc == 2)
			unparsed = all;
		if (argv[1][1] == '-' && !argv[1][2] && argc == 3)
			unparsed = argv[2];
	} else
		unparsed = argv[1];

	if (!unparsed) {
		usage(usage_msg);
		return 1;
	}

	parsed = parse_format(unparsed);
	parts_debug(parsed, unparsed);
	return 0;
}
