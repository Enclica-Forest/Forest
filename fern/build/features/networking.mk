# build/features/networking.mk
#
# Networking feature gating.
#
# Appends to EXCLUDED_CSOURCES (defined by build/kernel-sources.mk) when
# networking features are turned off. ENABLE_NETWORKING is the parent toggle:
# when it is `no`, the top-level net.c umbrella and every src/networking/*.c
# subsystem source are dropped. Per-protocol and per-driver toggles gate the
# individual files independently so a build can keep networking on but disable
# a single protocol (e.g. ENABLE_TCP=no drops only tcp.c).
#
# Source map (verified to exist under $(SRCDIR)/networking/):
#   tcp.c  -> ENABLE_TCP          udp.c   -> ENABLE_UDP
#   arp.c  -> ENABLE_ARP          icmp.c  -> ENABLE_ICMP
#   dhcp.c -> ENABLE_DHCP         dns.c   -> ENABLE_DNS
#   e1000.c   -> ENABLE_DRIVER_E1000
#   rtl8139.c -> ENABLE_DRIVER_RTL8139
#   ne2000.c  -> ENABLE_DRIVER_NE2000
#   ip.c netdev.c network.c -> always built when ENABLE_NETWORKING=yes
#
# Every reference uses $(wildcard ...) so a missing file never breaks the
# build; an unmatched glob (e.g. *ethernet*, or top-level tcp*.c which does
# not currently exist) simply contributes nothing to EXCLUDED_CSOURCES.
# build/config.mk guarantees each ENABLE_* is `yes` or `no`.
#
# QEMU networking (QEMU_NETWORK) is owned by build/qemu-run.mk, not here.

ifeq ($(ENABLE_NETWORKING),no)
EXCLUDED_CSOURCES += $(SRCDIR)/net.c $(wildcard $(SRCDIR)/networking/*.c)
endif

ifeq ($(ENABLE_ETHERNET),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/networking/*ethernet*.c)
endif

ifeq ($(ENABLE_TCP),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/networking/*tcp*.c) $(wildcard $(SRCDIR)/tcp*.c)
endif

ifeq ($(ENABLE_UDP),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/networking/*udp*.c) $(wildcard $(SRCDIR)/udp*.c)
endif

ifeq ($(ENABLE_DHCP),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/networking/*dhcp*.c) $(wildcard $(SRCDIR)/dhcp*.c)
endif

ifeq ($(ENABLE_DNS),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/networking/*dns*.c) $(wildcard $(SRCDIR)/dns*.c)
endif

ifeq ($(ENABLE_ARP),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/networking/*arp*.c) $(wildcard $(SRCDIR)/arp*.c)
endif

ifeq ($(ENABLE_ICMP),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/networking/*icmp*.c) $(wildcard $(SRCDIR)/icmp*.c)
endif

ifeq ($(ENABLE_DRIVER_E1000),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/networking/*e1000*.c) $(wildcard $(SRCDIR)/e1000*.c)
endif

ifeq ($(ENABLE_DRIVER_RTL8139),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/networking/*rtl8139*.c) $(wildcard $(SRCDIR)/rtl8139*.c)
endif

ifeq ($(ENABLE_DRIVER_NE2000),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/networking/*ne2000*.c) $(wildcard $(SRCDIR)/ne2000*.c)
endif

# End of build/features/networking.mk
