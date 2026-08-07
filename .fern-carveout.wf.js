export const meta = {
  name: 'fern-carveout',
  description: 'Carve Forest-OS into kernel-only "Fern": purge userspace/Canopy, full KERNEL_->FERN_ rename, keep foreboots bootloader, rebrand docs, verify build plumbing',
  phases: [
    { title: 'Survey', detail: 'read-only: map config keys, build fragments, src classification, branding surface' },
    { title: 'Carve', detail: '5 disjoint-file agents apply edits in parallel' },
    { title: 'Verify', detail: 'toolchain-free plumbing checks + real build attempt + contract greps' },
    { title: 'Repair', detail: 'fix failures reported by verify' },
  ],
}

const ROOT = '/home/bluet/forest'
const OS = ROOT + '/Forest-OS'

const HARD_FACTS = `
HARD FACTS (verified by orchestrator — do not re-litigate, obey):
- Repo root: ${ROOT}. Main OS tree: ${OS}. Bootloader: ${ROOT}/foreboots (KEEP — it is the ONLY userspace, it is the OS bootloader).
- Naming contract: "Forest-OS" = the whole operating system. "Fern" = ONLY the kernel component (like "Linux" the kernel vs a distro). Always refer to the kernel as Fern.
- Kernel artifact currently "kernel.elf"; rename to "fern.elf". Build vars/symbols KERNEL_* -> FERN_* (kernel-output-related ones) in build/config.mk, build/iso.mk, build/flags.mk, grub.cfg emission.
- dbus*, session.c, hotkey.c, xdg.c are IN-KERNEL (src/kernel.c #includes and calls them). DO NOT delete them blindly — they are kernel IPC/session-mgmt, not userspace apps.
- The userspace coupling to SEVER is: src/kernel.c's tail launches the Canopy GRAPHICAL DESKTOP session (launch_user_session / session_run / wm render loop). Kernel-only = do NOT auto-launch a desktop; end in a kernel-only state (existing in-kernel shell if present, else a clean halt/idle loop with a boot message). Prefer DISABLING desktop launch over deleting kernel files.
- No cross-toolchain guarantee for full link. When uncertain whether removing a src file breaks the link, DISABLE it via config/build gating rather than delete. Delete only clearly-desktop build glue (build/userspace.mk, build/features/canopy.mk) and clearly-userspace config keys.
- Build config flow (from AGENTS.md): .forestos_config (Kconfig, y/n) --[conf.sh --generate]--> build-config.mk (yes/no) --> Makefile includes build/*.mk. Key-name lists live in build/config.mk AND makeconfigs/config.mk. USERSPACE_*/CANOPY_* keys (39 of them) must be purged consistently across ALL of: .forestos_config, conf.sh, build/config.mk, makeconfigs/config.mk — a mismatch breaks conf.sh.
- Toolchain-free verification (no build needed): bash -n conf.sh ; ./conf.sh --defconfig ; make configcheck ; make -n build ; make show-config.
`

phase('Survey')
const SURVEY_SCHEMA = {
  type: 'object',
  required: ['domain', 'spec_markdown'],
  properties: {
    domain: { type: 'string' },
    spec_markdown: { type: 'string', description: 'Detailed, file-path-precise carve-out plan for this domain' },
    risks: { type: 'array', items: { type: 'string' } },
  },
}

const surveys = await parallel([
  () => agent(`${HARD_FACTS}
DOMAIN: CONFIG. Read (do NOT edit): ${OS}/.forestos_config, ${OS}/conf.sh, ${OS}/build/config.mk, ${OS}/makeconfigs/config.mk.
Produce an authoritative list of EVERY USERSPACE_* and CANOPY_* config key and EXACTLY where each appears (file + how: defconfig line, conf.sh menu/default/dependency, config.mk key-list). Also identify the dependency-resolution logic in conf.sh that forces children off. Deliver a precise removal plan so purging is consistent across all 4 files with zero dangling references. Note where a new kernel-only defconfig ("fern" defaults, no userspace/canopy) should be added.`,
    { label: 'survey:config', phase: 'Survey', schema: SURVEY_SCHEMA, agentType: 'general-purpose' }),

  () => agent(`${HARD_FACTS}
DOMAIN: BUILDSYS. Read (do NOT edit): ${OS}/Makefile, ${OS}/build/userspace.mk, ${OS}/build/features/canopy.mk, ${OS}/build/kernel-sources.mk, ${OS}/build/config.mk, ${OS}/build/iso.mk, ${OS}/build/flags.mk, ${OS}/build/foreb.mk.
Produce: (1) the exact Makefile include lines to remove (canopy.mk, userspace.mk) and files to delete; (2) every USERSPACE_EXCLUDED / userspace / canopy reference in kernel-sources.mk and other fragments that becomes dead and must be cleaned; (3) a COMPLETE rename map of kernel.elf->fern.elf and KERNEL_*->FERN_* occurrences (file:line, old->new) in config.mk/iso.mk/flags.mk/grub emission; (4) how foreboots (foreb.mk, ENABLE_FOREB_BOOTLOADER) should be presented as THE OS bootloader. Be exhaustive and line-precise.`,
    { label: 'survey:buildsys', phase: 'Survey', schema: SURVEY_SCHEMA, agentType: 'general-purpose' }),

  () => agent(`${HARD_FACTS}
DOMAIN: SRC. Read (do NOT edit): ${OS}/src/kernel.c (esp. the tail: boot sequence, launch_user_session, session_run, wm render loop, hotkey/xdg/session init) and these candidates: src/dbus*.c, src/session.c, src/xdg.c, src/hotkey.c, src/cgdm_integration.c, src/display_manager.c, src/mode_state.c. Grep src/ for who references each.
Classify each as KERNEL-CORE (keep), KERNEL-SESSION-GLUE (keep code, but ensure not auto-launching desktop), or PURE-DESKTOP (candidate remove). Deliver an EXACT edit plan for src/kernel.c to sever automatic graphical-desktop/user-session launch and instead end kernel-only (drop to in-kernel shell if one exists — search for it — else a labeled halt/idle loop). Favor disabling over deletion; for any file you propose deleting, prove via grep it is not referenced by kept kernel code. List every symbol kernel.c would lose and how to guard it.`,
    { label: 'survey:src', phase: 'Survey', schema: SURVEY_SCHEMA, agentType: 'general-purpose' }),

  () => agent(`${HARD_FACTS}
DOMAIN: BRANDING. Read (do NOT edit): ${OS}/README.md, ${OS}/AGENTS.md, ${OS}/.forestos_config (comment banners), any ${OS}/docs/*, ${ROOT}/foreboots/README.md.
Produce a precise plan to establish the vocabulary everywhere: "Forest-OS" = whole OS (codename ALDER stays as the OS codename), "Fern" = the kernel component. Every place that calls the kernel "Forest OS kernel" / "the kernel" as a product name should name it Fern. README should explain the component model (Fern kernel + foreboots bootloader = Forest-OS, no bundled userspace apps, POSIX-oriented). List file-by-file what wording changes.`,
    { label: 'survey:branding', phase: 'Survey', schema: SURVEY_SCHEMA, agentType: 'general-purpose' }),
])

const brief = surveys.filter(Boolean).map(s => `\n===== SURVEY: ${s.domain} =====\n${s.spec_markdown}\n${(s.risks||[]).length ? 'RISKS: ' + s.risks.join(' | ') : ''}`).join('\n')
log(`Survey complete: ${surveys.filter(Boolean).length}/4 domains mapped`)

phase('Carve')
const EDIT_SCHEMA = {
  type: 'object',
  required: ['owner', 'changes', 'files_touched'],
  properties: {
    owner: { type: 'string' },
    changes: { type: 'string', description: 'What was changed and why' },
    files_touched: { type: 'array', items: { type: 'string' } },
    deletions: { type: 'array', items: { type: 'string' } },
    followups: { type: 'array', items: { type: 'string' } },
  },
}

const MASTER = `${HARD_FACTS}\n\nMASTER CARVE-OUT BRIEF (from survey phase — authoritative):\n${brief}\n\nYou apply edits with Edit/Write/Bash. STAY STRICTLY within your assigned files — other agents own the rest concurrently. Make real edits, not suggestions.`

const edits = await parallel([
  // E1 CONFIG — .forestos_config, conf.sh, makeconfigs/config.mk (NOT build/config.mk; E2 owns that)
  () => agent(`${MASTER}
YOUR FILES ONLY: ${OS}/.forestos_config, ${OS}/conf.sh, ${OS}/makeconfigs/config.mk.
Task: purge ALL USERSPACE_* and CANOPY_* keys and their conf.sh menu entries/defaults/dependency logic from these three files, consistently. Keep in-kernel features (memory/fs/graphics driver/networking/usb/etc.) untouched. Add a kernel-only "fern" defconfig posture (no userspace/canopy). Do NOT touch build/config.mk. Verify with: bash -n ${OS}/conf.sh.`,
    { label: 'carve:config', phase: 'Carve', schema: EDIT_SCHEMA, agentType: 'general-purpose' }),

  // E2 BUILDSYS — build/config.mk, Makefile, build/kernel-sources.mk; delete userspace.mk + canopy.mk
  () => agent(`${MASTER}
YOUR FILES ONLY: ${OS}/build/config.mk, ${OS}/Makefile, ${OS}/build/kernel-sources.mk, and DELETE ${OS}/build/userspace.mk and ${OS}/build/features/canopy.mk.
Task: (1) remove the two include lines (features/canopy.mk, userspace.mk) from Makefile; (2) rm those two fragment files; (3) in build/config.mk remove USERSPACE_*/CANOPY_* from key-lists AND apply the KERNEL_*->FERN_* rename per the brief; (4) in build/kernel-sources.mk strip now-dead USERSPACE_EXCLUDED/userspace/canopy references. Do NOT touch .forestos_config/conf.sh/makeconfigs (E1) or iso.mk/flags.mk (E3). Keep compat.mk include (in-kernel). Sanity: grep that no removed include remains.`,
    { label: 'carve:buildsys', phase: 'Carve', schema: EDIT_SCHEMA, agentType: 'general-purpose' }),

  // E3 ARTIFACT — build/iso.mk, build/flags.mk, build/foreb.mk
  () => agent(`${MASTER}
YOUR FILES ONLY: ${OS}/build/iso.mk, ${OS}/build/flags.mk, ${OS}/build/foreb.mk.
Task: rename kernel.elf->fern.elf and KERNEL_*->FERN_* (kernel-output vars) throughout these files, including grub.cfg "multiboot2 /boot/kernel.elf" emission -> /boot/fern.elf. In foreb.mk update the kernel path/comments to fern and ensure foreboots is presented as the OS bootloader. Do NOT touch build/config.mk (E2 renames the copies there). Keep behavior identical apart from names.`,
    { label: 'carve:artifact', phase: 'Carve', schema: EDIT_SCHEMA, agentType: 'general-purpose' }),

  // E4 SRC — src/ only
  () => agent(`${MASTER}
YOUR FILES ONLY: ${OS}/src/ (primarily src/kernel.c).
Task: per the SRC survey, edit src/kernel.c to STOP auto-launching the Canopy graphical desktop/user session. Kernel-only end state: if an in-kernel shell exists, enter it; else a clearly-labeled boot message + safe idle/halt loop. Keep in-kernel dbus/session/hotkey/xdg symbols compiling (guard or no-op the desktop-launch calls; do not delete their source). Only delete a src file if the survey PROVED it is pure-desktop and unreferenced by kept code — otherwise disable via gating. Preserve all kernel subsystem init. Document every change inline with a brief comment. Report exactly what you changed and any symbol you had to stub.`,
    { label: 'carve:src', phase: 'Carve', schema: EDIT_SCHEMA, agentType: 'general-purpose' }),

  // E5 DOCS — README, AGENTS, docs, foreboots README
  () => agent(`${MASTER}
YOUR FILES ONLY: ${OS}/README.md, ${OS}/AGENTS.md, ${OS}/docs/ (if present), ${ROOT}/foreboots/README.md.
Task: rewrite branding to the component model — Fern = the kernel (POSIX-oriented, no bundled userspace apps), foreboots = the bootloader, together = Forest-OS (codename ALDER, the whole OS). Update the README build section to reflect kernel-only (fern.elf) + foreboots. Keep all build commands accurate to the current Makefile targets. Do NOT touch build files or src.`,
    { label: 'carve:docs', phase: 'Carve', schema: EDIT_SCHEMA, agentType: 'general-purpose' }),
])
log(`Carve complete: ${edits.filter(Boolean).length}/5 edit agents finished`)

phase('Verify')
const VERIFY_SCHEMA = {
  type: 'object',
  required: ['check', 'passed', 'details'],
  properties: {
    check: { type: 'string' },
    passed: { type: 'boolean' },
    details: { type: 'string' },
    errors: { type: 'array', items: { type: 'string' } },
  },
}

const verifies = await parallel([
  () => agent(`${HARD_FACTS}
VERIFY: build-config plumbing (toolchain-free). Run in ${OS}:
  bash -n conf.sh ; ./conf.sh --defconfig ; make configcheck ; make -n build ; make show-config
Report pass/fail and capture any error output verbatim (first ~40 lines). passed=true only if all commands exit 0 and make -n build resolves all includes with no missing-file/undefined errors.`,
    { label: 'verify:plumbing', phase: 'Verify', schema: VERIFY_SCHEMA, agentType: 'general-purpose' }),

  () => agent(`${HARD_FACTS}
VERIFY: contract greps in ${OS}. Confirm ZERO dangling references:
  - no USERSPACE_ or CANOPY_ tokens remain in .forestos_config, conf.sh, build/config.mk, makeconfigs/config.mk, build/*.mk, Makefile
  - no "kernel.elf" remains (all -> fern.elf); no stray KERNEL_* kernel-output var that should be FERN_*
  - Makefile does not include build/userspace.mk or build/features/canopy.mk; those files are gone
  - build/features/compat.mk include still present (kept)
Use grep -rn. List every offending hit as an error. passed=true only if clean.`,
    { label: 'verify:contracts', phase: 'Verify', schema: VERIFY_SCHEMA, agentType: 'general-purpose' }),

  () => agent(`${HARD_FACTS}
VERIFY: real build attempt (best-effort). Toolchain at ${ROOT}/forestos-toolchain/install/bin (i686-elf-*). In ${OS} run: ./conf.sh --defconfig then attempt \`make ARCH=32 -k -j4\` (or the documented kernel target) with a hard timeout ~240s. Capture the FIRST ~30 compile/link errors. This is informational: passed=true if it builds fern.elf; if it fails, set passed=false and put the concrete errors in errors[] so Repair can act. Do NOT fix anything yourself.`,
    { label: 'verify:build', phase: 'Verify', schema: VERIFY_SCHEMA, agentType: 'general-purpose', effort: 'high' }),
])

const failures = verifies.filter(Boolean).filter(v => !v.passed)
log(`Verify: ${verifies.filter(Boolean).length - failures.length}/${verifies.filter(Boolean).length} checks passed`)

phase('Repair')
let repairReport = 'No repair needed — all checks passed.'
if (failures.length) {
  const failText = failures.map(f => `### ${f.check}\n${f.details}\nERRORS:\n- ${(f.errors||[]).join('\n- ')}`).join('\n\n')
  const rep = await agent(`${HARD_FACTS}
The carve-out is applied but verification found failures. Fix them in ${OS}. Prefer minimal, correct fixes consistent with the naming/contract rules above. For build/link errors from severed userspace-desktop launch, guard/stub rather than reintroduce desktop launch. After fixing, re-run the failing checks to confirm. Do NOT reintroduce userspace/Canopy config keys or the deleted includes.

FAILURES:
${failText}`,
    { label: 'repair', phase: 'Repair', schema: {
      type: 'object', required: ['fixed', 'summary'],
      properties: { fixed: { type: 'boolean' }, summary: { type: 'string' }, remaining: { type: 'array', items: { type: 'string' } } },
    }, agentType: 'general-purpose', effort: 'high' })
  repairReport = rep ? `fixed=${rep.fixed}\n${rep.summary}${rep.remaining && rep.remaining.length ? '\nREMAINING: ' + rep.remaining.join(' | ') : ''}` : 'Repair agent returned no result.'
}

return {
  surveys: surveys.filter(Boolean).map(s => s.domain),
  edits: edits.filter(Boolean).map(e => ({ owner: e.owner, files: e.files_touched, deletions: e.deletions || [] })),
  verify: verifies.filter(Boolean).map(v => ({ check: v.check, passed: v.passed })),
  repair: repairReport,
}
