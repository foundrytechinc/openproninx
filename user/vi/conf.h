/* neatvi configuration file */

/* access mode of new files */
#define MKFILE_MODE	0600

/* map file names to file types */
static struct filetype {
	char *ft;		/* file type */
	char *pat;		/* file name pattern */
	char *def;		/* pattern for global definitions (for gd command) */
	char *sec;		/* section start pattern (for [[ and ]] commands) */
} filetypes[] = {
	{"c", "\\.[hc]$", "^([a-zA-Z_].*)?\\<%s\\>.*([^;]|[^)];)$", 0},
	{"roff", "\\.(ms|me|mom|tr|roff|tmac|[1-9])$", "^\\.(de|nr|ds) +%s\\>", 0},
	{"tex", "\\.tex$", 0, 0},
	{"msg", "letter$|mbox$|mail$", 0, 0},
	{"mk", "Makefile$|makefile$|\\.mk$", "^%s:", 0},
	{"sh", "\\.sh$", "^(function +)?%s(\\(\\))? *\\{", "^(function +)?[a-zA-Z_0-9]+(\\(\\))? *\\{$"},
	{"go", "\\.go$", "^(func|var|const|type)( +\\(.*\\))? +%s\\>", "^(func|type)\\>.*\\{$"},
	{"py", "\\.py$", "^(def|class) +\\<%s\\>", "^(def|class) "},
	{"bib", "bib$", 0, 0},
	{"nm", "\\.nm$", 0, 0},
	{"diff", "\\.(patch|diff)$", 0, 0},
	{"ls", "ls$", 0, 0},
	{"ex", "\\.neatvi$", 0, 0},
	{"txt", "$", 0, 0},		/* matches everything; must be the last pattern */
};

#define SM(c)		(128 + c)
#define SX(c)		(256 + c)

/* syntax highlighting patterns */
static struct highlight {
	char *ft;		/* the filetype of this pattern */
	int att[16];		/* attributes of the matched groups */
	char *pat;		/* regular expression */
	int end;		/* the group ending this pattern */
} highlights[] = {
	/* status bar */
	{"---", {'_', 0, 'N', 'I', 'L', 'W', 'F', 'K', 'T', 'C'},
		"^((N)|(I))([0-9]+) +([^ ]) +([^ ]+) +([^ ]+) +([0-9]+) +C([0-9]+).*$", 0},
	{"---", {'_', 'W', 'L', 'W', 'F'}, "^([WR])([0-9]+) ([^ ])([^ ]*).*$", 0},
	{"---", {'!'}, "^E .*$", 0},
	{"---", {'U', '1', 'D', 0, 'B', '2', '2'},
		"^\\[([-0-9])\\] +(/?([^ /:]+/+)*)([^ /:]+)(:[0-9]+:)?(.*)$", 0},
	{"---", {'U'}, "^/?\\[-\\].*$", 0},
	{"---", {'Q', 'N', 'F'}, "^LEAP ([A-Z]+) +[^ ]+ +([^ ]+)[^\n]*\n?", 0},
	{"---", {'_'}, "^.*$\n?", 0},
	/* ex mode */
	{"-ex", {':'}, "^[:/?!].*$", 0},
	{"-ex", {'X'}, "^.*$\n?", 0},

	/* C */
	{"c", {'t'}, "\\<(signed|unsigned|char|short|int|long|float|double|void|struct|enum|union|typedef)\\>", 0},
	{"c", {'k'}, "\\<(static|extern|register)\\>", 0},
	{"c", {'r'}, "\\<(return|for|while|if|else|do|sizeof|goto|switch|case|default|break|continue)\\>", 0},
	{"c", {'c'}, "//.*$", 0},
	{"c", {'c'}, "/\\*([^*]|\\*+[^*/])*\\*+/", 0},
	{"c", {'m', 'p'}, "^#([ \t]*include).*", 0},
	{"c", {'m', 'p'}, "^#([ \t]*[a-zA-Z0-9_]+)", 0},
	{"c", {0, 'f'}, "([a-zA-Z][a-zA-Z0-9_]+)\\(", 1},
	{"c", {'s'}, "\"([^\"\\]|\\\\.)*\"", 0},
	{"c", {'h'}, "'([^\\]|\\\\.)'", 0},
	{"c", {'0'}, "[-+]?\\<(0[xX][0-9a-fA-F]+|[0-9]+)\\>", 0},
	{"c", {'c'}, "^\t*(/\\*.*| \\*.*| \\*\\//)$", 0},
	{"c", {'i'}, "[a-zA-Z][a-zA-Z0-9_]*", 0},

	/* troff */
	{"roff", {0, 'k', 'd'}, "^[.'][ \t]*(SH)(.*)$", 0},
	{"roff", {0, 'k', 'd'}, "^[.'][ \t]*de (.*)$", 0},
	{"roff", {0, 'b'}, "^[.'][ \t]*([^ \t\\]*)", 1},
	{"roff", {'s'}, "\"([^\"]|\"\")*\"?", 0},
	{"roff", {'c'}, "\\\\\".*$", 0},
	{"roff", {'b'}, "\\\\{1,2}[*$fgkmns]([^[(]|\\(..|\\[[^]]*\\])", 0},
	{"roff", {'v'}, "\\\\([^[(*$fgkmns]|\\(..|\\[[^]]*\\])", 0},
	{"roff", {'s'}, "\\$[^$]+\\$", 0},
	{"roff", {'0'}, "[-+]?\\<([0-9]+(\\.[0-9]+)?)\\>", 0},

	/* tex */
	{"tex", {0, 'k', 0, 0, 'm', 0, 'f'},
		"\\\\([^[{ \t]+)((\\[([^][]+)\\])|(\\{([^}]*)\\}))*", 0},
	{"tex", {'s'}, "\\$[^$]+\\$", 0},
	{"tex", {'c'}, "%.*$", 0},

	/* mail */
	{"msg", {SM('h')}, "^From .*20..$", 0},
	{"msg", {SM('h'), SM('s')}, "^Subject: (.*)$", 0},
	{"msg", {SM('h'), SM('f')}, "^From: (.*)$", 0},
	{"msg", {SM('h'), SM('t')}, "^To: (.*)$", 0},
	{"msg", {SM('h'), SM('c')}, "^Cc: (.*)$", 0},
	{"msg", {SM('h')}, "^[A-Z][-A-Za-z]+: .+$", 0},
	{"msg", {SM('r')}, "^> .*$", 0},

	/* makefile */
	{"mk", {0, 'd'}, "^([A-Za-z_][A-Za-z0-9_]*)[ \t]*=", 0},
	{"mk", {'v'}, "\\$\\([a-zA-Z0-9_]+\\)", 0},
	{"mk", {'c'}, "#.*$", 0},
	{"mk", {0, 'f'}, "([A-Za-z_%.]+):", 0},

	/* shell script */
	{"sh", {0, 'k', 'd'}, "^(function +)?([a-zA-Z_0-9]+) *(\\(\\))? *\\{", 0},
	{"sh", {'r'}, "\\<(break|case|continue|do|done|elif|else|esac|fi|for|if|in|then|until|while|return)\\>", 0},
	{"sh", {'s'}, "\"([^\"\\]|\\\\.)*\"", 0},
	{"sh", {'s'}, "'[^']*'", 0},
	{"sh", {'s'}, "`([^`\\]|\\\\.)*`", 0},
	{"sh", {'v'}, "\\$(\\{[^}]+\\}|[a-zA-Z_0-9]+|[!#$?*@-])", 0},
	{"sh", {'v'}, "\\$\\([^()]+\\)", 0},
	{"sh", {'f'}, "^\\. .*$", 0},
	{"sh", {'c'}, "#.*$", 0},

	/* go */
	{"go", {0, 'k', 'i', 'd'}, "^\\<(func) (\\([^()]+\\) )?([a-zA-Z0-9_]+)\\>", 0},
	{"go", {'k'}, "\\<(func|type|var|const|package)\\>", 0},
	{"go", {'p', 'm'}, "^import[ \t]+([^ ]+)", 0},
	{"go", {'k'}, "\\<(import|interface|struct)\\>", 0},
	{"go", {'r'}, "\\<(break|case|chan|continue|default|defer|else|fallthrough|for|go|goto|if|map|range|return|select|switch)\\>", 0},
	{"go", {0, 'b'}, "\\<(append|copy|delete|len|cap|make|new|complex|real|imag|close|panic|recover|print|println|int|int8|int16|int32|int64|uint|uint8|uint16|uint32|uint64|uintptr|float32|float64|complex128|complex64|bool|byte|rune|string|error)\\>\\(", 0},
	{"go", {'t'}, "\\<(true|false|iota|nil|int8|int16|int32|int64|int|uint8|uint16|uint32|uint64|uint|uintptr|float32|float64|complex128|complex64|bool|byte|rune|string|error)\\>", 0},
	{"go", {'c'}, "//.*$", 0},
	{"go", {'c'}, "/\\*([^*]|\\*+[^*/])*\\*+/", 0},
	{"go", {0, 'f'}, "([a-zA-Z][a-zA-Z0-9_]*)\\(", 1},
	{"go", {'i'}, "[a-zA-Z][a-zA-Z0-9_]*", 0},
	{"go", {'s'}, "\"([^\"\\]|\\\\.)*\"", 0},
	{"go", {'h'}, "'([^']|\\\\')*'", 0},
	{"go", {'s'}, "`([^`]|\\\\`)*`", 0},
	{"go", {'0'}, "[-+]?\\<(0[xX][0-9a-fA-F]+|[0-9.]+)\\>", 0},
	{"go", {'c'}, "^\t*(/\\*.*| \\*.*| \\*\\//)$", 0},

	/* refer */
	{"bib", {0, SM('h'), SM('l')}, "^(%L) +(.*)$", 1},
	{"bib", {0, SM('h'), SM('f')}, "^(%A) (.*)$", 1},
	{"bib", {0, SM('h'), SM('s')}, "^(%T) (.*)$", 1},
	{"bib", {0, SM('h'), SM('t')}, "^(%[JB]) (.*)$", 1},
	{"bib", {0, SM('h')}, "^(%[A-Z]) (.*)$", 1},
	{"bib", {'c'}, "^#.*$", 1},

	/* python */
	{"py", {'c'}, "#.*$", 0},
	{"py", {'k'}, "\\<(class|def)\\>", 0},
	{"py", {'k'}, "\\<(and|or|not|is|in)\\>", 0},
	{"py", {0, 0, 'p', 'i', 'p', 'm'}, "^((from)[ \t]+([^ ]+)[ \t]+)?(import)[ \t]+([^ ]+)", 0},
	{"py", {'k'}, "\\<(import|from|global|lambda|del)\\>", 0},
	{"py", {'r'}, "\\<(for|while|if|elif|else|pass|return|break|continue)\\>", 0},
	{"py", {'r'}, "\\<(try|except|as|raise|finally|with)\\>", 0},
	{"py", {0, 'f'}, "([a-zA-Z][a-zA-Z0-9_]+)\\(", 1},
	{"py", {'s'}, "[\"']([^\"'\\]|\\\\.)*[\"']", 0},
	{"py", {'i'}, "[a-zA-Z][a-zA-Z0-9_]*", 0},

	/* neatmail listing */
	{"nm", {0, SM('S'), SM('0'), SM('M'), SM('1'), SM('R'), SM('U')},
		"^([ROU][^ABCDHML0-9]?)(0*)([0-9]+)(@[^ \t]*)? *\t\\[([^\t]*)\\]\t *\\[([^\t]*)\\].*", 0},
	{"nm", {SM('N')}, "^[N].*$", 0},
	{"nm", {SM('A')}, "^[A-Z][HA].*$", 0},
	{"nm", {SM('B')}, "^[A-Z][MB].*$", 0},
	{"nm", {SM('C')}, "^[A-Z][C].*$", 0},
	{"nm", {SM('D')}, "^[A-Z][LD].*$", 0},
	{"nm", {SM('F')}, "^[F].*$", 0},
	{"nm", {SM('I')}, "^\t.*$", 0},
	{"nm", {SM('X')}, "^:.*$", 0},

	/* diff */
	{"diff", {SX('-')}, "^-.*$", 0},
	{"diff", {SX('+')}, "^\\+.*$", 0},
	{"diff", {SX('@')}, "^@.*$", 0},
	{"diff", {SX('D')}, "^diff .*$", 0},

	/* directory listing */
	{"ls", {0, SX('d'), 0, SX('b'), SX('n'), SX('s'), SX('l')},
		"^(/?([^#/: \t]+/)*)([^/: \t]+)\\>(:[0-9]+:)?([^\t]+\t)?(.*)$", 0},
	{"ls", {SX('c')}, "^#.*$", 0},

	/* .neatvi */
	{"ex", {'c'}, "\".*$", 0},
	{"ex", {'b', 'd'}, "^rs[ \t]+([^ ]+)", 0},
	{"ex", {0, 'b', 'f'}, "^[ \t]*(@|rs|ra|rx|rk)[ \t]+([^ ]+)", 0},
	{"ex", {0, '0', 0, 'b'}, "^[ \t]*(([-0-9:%,;.$+]|'.|`.|/[^/]*/|\\?[^?]*\\?)*)[ \t]*([a-z!]+)", 0},
	{"ex", {'r'}, "^\\.$", 0},
};

/* right-to-left characters (used only in dircontexts[] and dirmarks[]) */
#define CR2L		"ءآأؤإئابةتثجحخدذرزسشصضطظعغـفقكلمنهوىييپچژکگیۀ‌‍؛،»«؟ًٌٍَُِّْٔ"
/* neutral characters (used only in dircontexts[] and dirmarks[]) */
#define CNEUT		"-!\"#$%&'()*+,./:;<=>?@^_`{|}~ "

/* direction context; specifies the base direction of lines */
static struct dircontext {
	int dir;
	char *pat;
} dircontexts[] = {
	{-1, "^[" CR2L "]"},
	{+1, "^[a-zA-Z_0-9]"},
};

/* direction marks; the direction of contiguous characters in a line */
static struct dirmark {
	int ctx;	/* the direction context for this mark; 0 means any */
	int dir;	/* the direction of the matched text */
	int grp;	/* the nested subgroup; 0 means no groups */
	char *pat;
} dirmarks[] = {
	{+0, +1, 1, "\\\\\\*\\[([^]]+)\\]"},
	{+1, -1, 0, "[" CR2L "][" CNEUT CR2L "]*[" CR2L "]"},
	{-1, +1, 0, "[a-zA-Z0-9_][^" CR2L "\\\\`$']*[a-zA-Z0-9_]"},
	{+0, +1, 0, "\\$([^$]+)\\$"},
	{+0, +1, 1, "\\\\[a-zA-Z0-9_]+\\{([^}]+)\\}"},
	{-1, +1, 0, "\\\\[^ \t" CR2L "]+"},
};

/* external commands */
#define ECMD	"neatvi.sh"
