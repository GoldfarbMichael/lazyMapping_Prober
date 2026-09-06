# clock_labels.sh -- the timer-mode naming table, in ONE place.
#
# Sourced, never executed. Two questions, one answer each:
#   clock_subdir_for <TIMER_MODE> <SHUFFLE_FLAG> <JSMAP_BUF_MB>  -> on-disk tree name
#   clock_label_for  <TIMER_MODE> <SHUFFLE_FLAG>                 -> friendly .h5 label
#
# The subdir MUST mirror timer_mode_subdir() in src/mastikElite.c exactly -- that C function
# decides where the CSVs are actually written, and this decides where the shell goes looking
# for them. A drift between the two means the finalizer reports "config dir missing" on data
# that exists, or worse, packs the wrong tree.
#
# Previously this table was copy-pasted into batch_runner.sh, finalize_experiment.sh and
# (nearly) run_all_configs.sh. Adding the website modes made that a four-way duplication with
# no test, so it lives here now.
#
# NOTE: batch_runner.sh still carries its own copy for TIMER_SUBDIR; migrate it here when
# convenient (it is the same table).

# Base tree name per timer mode. `default` covers -n and anything unrecognised, matching the C.
clock_subdir_for() {
    local mode="$1" shuffle="${2:-}" buf="${3:-12}" base
    case "$mode" in
        -c)     base="chrome_clock" ;;
        -j)     base="chrome_clock_jsmap" ;;
        -jn)    base="native_clock_jsmap" ;;
        -jb)    base="chrome_clock_jsmap_bidir" ;;
        -jnb)   base="native_clock_jsmap_bidir" ;;
        -jss)   base="chrome_clock_jsmapSS" ;;
        -jssb)  base="chrome_clock_jsmapSS_bidir" ;;
        -jnss)  base="native_clock_jsmapSS" ;;
        -jnssb) base="native_clock_jsmapSS_bidir" ;;
        -wn)    base="native_clock_website" ;;
        -wc)    base="chrome_clock_website" ;;
        *)      base="native_clock" ;;
    esac
    # The shuffled Mastik-e_set A/B writes to its own tree so chrome_clock/ is never overwritten.
    if [ "$mode" = "-c" ] && [ "$shuffle" = "-s" ]; then base="chrome_clock_shuffled"; fi
    # Only the lazy-map modes have a victim buffer to tag; a non-default size gets its own tree
    # so a 24 MB run can never overwrite 12 MB data.
    case "$mode" in
        -j|-jn|-jb|-jnb|-jss|-jssb|-jnss|-jnssb)
            if [ "$buf" != "12" ]; then base="${base}_${buf}MB"; fi ;;
    esac
    printf '%s' "$base"
}

# Friendly label for the .h5 filename (and the remote status log). Distinct from the on-disk
# tree name, and MUST stay disjoint from the JS arms' "website"/"realbrowser" labels:
# finalize_lib.sh's rerun counter matches ^<LABEL>[0-9]*<NAME_TAIL>$, so a colliding prefix
# would renumber the wrong series.
clock_label_for() {
    local mode="$1" shuffle="${2:-}" label
    case "$mode" in
        -c)     label="chrome" ;;
        -j)     label="chromeJSmap" ;;
        -jn)    label="nativeJSmap" ;;
        -jb)    label="chromeJSmapBidir" ;;
        -jnb)   label="nativeJSmapBidir" ;;
        -jss)   label="chromeJSmapSS" ;;
        -jssb)  label="chromeJSmapSSBidir" ;;
        -jnss)  label="nativeJSmapSS" ;;
        -jnssb) label="nativeJSmapSSBidir" ;;
        -wn)    label="mastikWebsiteNative" ;;
        -wc)    label="mastikWebsiteChrome" ;;
        *)      label="native" ;;
    esac
    if [ "$mode" = "-c" ] && [ "$shuffle" = "-s" ]; then label="chromeShuffled"; fi
    printf '%s' "$label"
}
