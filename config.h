/* hnd configuration. Edit, then rebuild with `make`. Width settings can
 * be overridden at runtime by the -w/-W flags; everything else is
 * compiled in. */
static unsigned int minwidth = 380;      /* narrowest notification (-w) */
static unsigned int maxwpct = 50;        /* widest, % of monitor width (-W) */
static const unsigned int borderpx = 2;  /* border width */
static const unsigned int padding = 8;   /* gap from screen corner */
static const unsigned int gap = 8;       /* gap between notifications */
static const unsigned int hpad = 10;     /* inner horizontal padding */
static const unsigned int vpad = 8;      /* inner vertical padding */
static const unsigned int linespc = 2;   /* extra px between lines */
static const unsigned int maxlines = 16; /* lines per notification cap */
static const int atbottom = 0;           /* stack from the bottom edge */
static const char fontname[] =
    "Iosevka NFM:pixelsize=13"; /* fontconfig name, "monospace" as fallback */
/* default timeout ms per urgency when the sender leaves it up to us;
 * 0 = sticky until clicked */
static const long defaulttimeout[3] = {5000, 5000, 0};
/* bg, summary fg, body fg, border - per urgency */
static const char *colors[3][4] = {
    {"#1a1b26", "#565f89", "#414868", "#3b4261"}, /* low */
    {"#1a1b26", "#c0caf5", "#7aa2f7", "#3b4261"}, /* normal */
    {"#1a1b26", "#f7768e", "#a9b1d6", "#f7768e"}, /* critical */
};
