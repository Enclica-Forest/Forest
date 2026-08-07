// Opt.h - a tiny std::optional-like tri-state wrapper.
//
// Every OPTIONAL forebo.cfg key is modeled as Opt<T>: a value plus a "set"
// flag. This mirrors the firmware's inherit sentinels (-1 for ints/enums/bools,
// FOREB_COLOR_UNSET for colors in uefi/config.c): an unset key must be OMITTED
// from the emitted file, never written as 0, so a round-trip does not inflate
// the config with defaults.
#ifndef FORB_OPT_H
#define FORB_OPT_H

template <class T>
struct Opt {
    T    v{};
    bool set = false;

    Opt() = default;
    Opt(const T &val) : v(val), set(true) {}

    bool isSet() const { return set; }
    void unset() { set = false; v = T{}; }
    void assign(const T &x) { v = x; set = true; }
    T    value(const T &fallback) const { return set ? v : fallback; }

    bool operator==(const Opt &o) const {
        if (set != o.set) return false;
        return !set || v == o.v;
    }
    bool operator!=(const Opt &o) const { return !(*this == o); }
};

#endif // FORB_OPT_H
