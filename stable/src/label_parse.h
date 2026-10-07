#ifndef LABEL_PARSE_H
#define LABEL_PARSE_H

// Config-label parsers ({NoC}C_{TST}TST_{K}K_{CYCLES}cycles). parse_cycles mirrors the JS
// CYCLES_PER_ADDRESS field (main.js LABEL_RE) and drives SST_cycles for every timer_mode.
// parse_TST overrides the TST_SEC default, so the label's TST field is authoritative too.
// Each returns -1 when its field is absent/malformed; parse_K may also return a VALID 0.
int parse_NoC_from_dirname(const char *dirname);
int parse_K_from_dirname(const char *dirname);
int parse_TST_from_dirname(const char *dirname);
int parse_cycles_from_dirname(const char *dirname);

#endif // LABEL_PARSE_H
