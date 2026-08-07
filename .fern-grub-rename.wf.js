export const meta = {
  name: 'fern-grub-rename',
  description: 'Strip GRUB from the Fern kernel build (foreboots is the bootloader) and rename dir Forest-OS -> fern',
  phases: [
    { title: 'Survey', detail: 'map GRUB surface + folder-rename path risks' },
    { title: 'Strip', detail: '3 disjoint agents remove GRUB, wire foreboots as the boot path' },
    { title: 'Rename', detail: 'git mv Forest-OS -> fern, fix path refs (single agent, after barrier)' },
    { title: 'Verify', detail: 'make -n build links fern.bin, no grub tokens, foreboots path resolves' },
  ],
}

const ROOT = '/home/bluet/forest'
const OS = ROOT + '/Forest-OS'   // pre-rename path; Rename phase moves it to ROOT/fern

const FACTS = `
HARD FACTS (verified by orchestrator — obey, do not re-litigate):
- Repo root ${ROOT}. Kernel build tree currently ${OS} (this dir is renamed to ${ROOT}/fern in the Rename phase — it holds the Fern kernel).
- "Forest-OS" as a PRODUCT NAME stays everywhere (the whole OS is still called Forest-OS). Only the DIRECTORY name Forest-OS changes to "fern". Do NOT rewrite branding strings like "Forest-OS effective build configuration" — those are correct. Only fix literal path references that break when the directory is renamed.
- foreboots is at ${ROOT}/foreboots — a SIBLING of the kernel dir, NOT inside it. It is THE bootloader (raw-MBR/UEFI), replacing GRUB entirely. build/foreb.mk currently sets FOREBO_DIR := \$(REPO_ROOT)/foreboots which wrongly points INSIDE the kernel dir; REPO_ROOT = \$(abspath \$(CURDIR)) = the kernel dir. Fix so it resolves the sibling ${ROOT}/foreboots (e.g. \$(REPO_ROOT)/../foreboots or \$(abspath \$(REPO_ROOT)/../foreboots)).
- GRUB is redundant and must be removed. GRUB touch points: build/iso.mk (grub.cfg emission + grub-mkrescue ISO rules), build/config.mk, build/dirs.mk, build/qemu-run.mk (run targets that -cdrom the grub ISO), Makefile, conf.sh, .forestos_config (CONFIG_GRUB_TIMEOUT key), and the Grub/ dir (grub.cfg, forebo.cfg, README.md).
- After stripping GRUB, the boot/run path MUST route through foreboots: set CONFIG_ENABLE_FOREB_BOOTLOADER default y, make \`make run\`/\`make iso\`(or their replacement) build+run via foreboots (forebo/forebo-image/forebo-qemu targets in build/foreb.mk). Keep producing the kernel artifact fern.bin/fern.elf.
- makeconfigs/ is a DEAD mirror (included nowhere) — ignore it entirely.
- Relative symlinks fern/libs -> ../libs and fern/forestos-toolchain -> ../forestos-toolchain already exist and survive the rename; do not break them.
- Toolchain-free verify: bash -n conf.sh ; ./conf.sh --defconfig ; make configcheck ; make -n build (must still link fern.bin) ; make show-config.
- Config-key purge must stay CONSISTENT across .forestos_config, conf.sh, build/config.mk (a mismatch breaks conf.sh). Same discipline as the prior carve-out.
`

phase('Survey')
const SUR = { type: 'object', required: ['domain', 'plan'], properties: { domain: { type: 'string' }, plan: { type: 'string' }, risks: { type: 'array', items: { type: 'string' } } } }

const surveys = await parallel([
  () => agent(`${FACTS}
DOMAIN: GRUB. Read (do NOT edit) in ${OS}: build/iso.mk, build/qemu-run.mk, build/config.mk, build/dirs.mk, Makefile, conf.sh, .forestos_config, and ls Grub/. Produce a line-precise plan: every grub reference and how to remove it; what replaces the ISO/run targets using foreboots; which config keys (GRUB_TIMEOUT etc.) to purge and from which files; whether Grub/ dir should be deleted. Identify anything foreboots needs that grub currently provides (e.g. the kernel path handed to the bootloader).`,
    { label: 'survey:grub', phase: 'Survey', schema: SUR, agentType: 'general-purpose' }),
  () => agent(`${FACTS}
DOMAIN: RENAME+FOREBOOTS. Read (do NOT edit): ${OS}/build/foreb.mk, ${OS}/build/dirs.mk, ${OS}/build/toolchain.mk, and grep -rn "Forest-OS" ${OS} plus grep for absolute paths containing Forest-OS. Classify every "Forest-OS" hit as PATH-REF (breaks on dir rename -> must fix) vs BRANDING-STRING (keep). Produce the exact foreb.mk edit to resolve the sibling ${ROOT}/foreboots, the plan to enable foreboots as default boot path (ENABLE_FOREB_BOOTLOADER=y), and the safe git-mv rename procedure with any residual path fixes.`,
    { label: 'survey:rename', phase: 'Survey', schema: SUR, agentType: 'general-purpose' }),
])
const brief = surveys.filter(Boolean).map(s => `\n===== ${s.domain} =====\n${s.plan}\n${(s.risks||[]).length ? 'RISKS: ' + s.risks.join(' | ') : ''}`).join('\n')
log(`Survey: ${surveys.filter(Boolean).length}/2 mapped`)

const MASTER = `${FACTS}\n\nMASTER PLAN (from survey — authoritative):\n${brief}\n\nApply real edits with Edit/Write/Bash. STAY within your assigned files; other agents own the rest concurrently.`
const EDIT = { type: 'object', required: ['owner', 'changes'], properties: { owner: { type: 'string' }, changes: { type: 'string' }, files: { type: 'array', items: { type: 'string' } }, followups: { type: 'array', items: { type: 'string' } } } }

phase('Strip')
const strips = await parallel([
  // S1: iso.mk + Grub/ dir + qemu-run.mk (the ISO/run/grub image path)
  () => agent(`${MASTER}
YOUR FILES ONLY: ${OS}/build/iso.mk, ${OS}/build/qemu-run.mk, and DELETE ${OS}/Grub/ (rm -rf).
Task: remove all GRUB ISO generation (grub.cfg emission, grub-mkrescue) from iso.mk and reroute the image/run rules to foreboots (use the forebo targets / FOREBO_DIR from build/foreb.mk which agent S3 fixes). \`make run\` must boot the foreboots image, not a grub cdrom. Keep producing fern.bin/fern.elf. Do NOT touch config.mk/dirs.mk/Makefile/conf.sh/.forestos_config (S2) or foreb.mk (S3).`,
    { label: 'strip:iso', phase: 'Strip', schema: EDIT, agentType: 'general-purpose' }),
  // S2: config surface + GRUB_TIMEOUT key purge
  () => agent(`${MASTER}
YOUR FILES ONLY: ${OS}/build/config.mk, ${OS}/build/dirs.mk, ${OS}/Makefile, ${OS}/conf.sh, ${OS}/.forestos_config.
Task: purge the GRUB_TIMEOUT config key (and any other grub-only keys) consistently from .forestos_config, conf.sh (CONFIG_DB + CATEGORIES), build/config.mk key-lists; remove grub var defs in dirs.mk/config.mk and grub-related bits in Makefile. Ensure ENABLE_FOREB_BOOTLOADER defaults to y (foreboots is now the only bootloader). Do NOT remove Forest-OS branding strings. Do NOT touch iso.mk/qemu-run.mk (S1) or foreb.mk (S3). Verify bash -n conf.sh.`,
    { label: 'strip:config', phase: 'Strip', schema: EDIT, agentType: 'general-purpose' }),
  // S3: foreb.mk sibling resolution + make foreboots the boot path
  () => agent(`${MASTER}
YOUR FILES ONLY: ${OS}/build/foreb.mk.
Task: fix FOREBO_DIR to resolve the SIBLING ${ROOT}/foreboots (currently \$(REPO_ROOT)/foreboots points inside the kernel dir). Use \$(abspath \$(REPO_ROOT)/../foreboots). Ensure the forebo/forebo-image/forebo-qemu recipes hand the built Fern kernel (fern.bin/fern.elf, via \$(OUTPUT)) to foreboots correctly. Update comments to reflect foreboots is THE (only) bootloader now that GRUB is gone. Do NOT touch other files.`,
    { label: 'strip:foreb', phase: 'Strip', schema: EDIT, agentType: 'general-purpose' }),
])
log(`Strip: ${strips.filter(Boolean).length}/3 done`)

// BARRIER already passed (parallel awaited). Now rename — single agent, no races.
phase('Rename')
const renameRes = await agent(`${MASTER}
All GRUB-strip edits are applied. Now perform the directory rename in ${ROOT}:
1. \`git -C ${ROOT} mv Forest-OS fern\` (preserves history; the two relative symlinks fern/libs->../libs and fern/forestos-toolchain->../forestos-toolchain must still resolve — verify with ls -l).
2. Fix ONLY residual literal path references broken by the rename (grep -rn "Forest-OS/" and absolute "${OS}" strings in fern/ build files and any scripts). Leave "Forest-OS" branding/product-name strings untouched.
3. Confirm foreb.mk's sibling foreboots path still resolves (it is ../foreboots relative to REPO_ROOT, unaffected by the rename).
Report exactly what moved and what path refs you fixed. Use Bash for git mv and grep; Edit for fixes.`,
  { label: 'rename:git-mv', phase: 'Rename', schema: { type: 'object', required: ['moved', 'summary'], properties: { moved: { type: 'boolean' }, summary: { type: 'string' }, path_fixes: { type: 'array', items: { type: 'string' } } } }, agentType: 'general-purpose', effort: 'high' })
log(`Rename: moved=${renameRes ? renameRes.moved : 'FAILED'}`)

phase('Verify')
const FERN = ROOT + '/fern'
const VER = { type: 'object', required: ['check', 'passed', 'details'], properties: { check: { type: 'string' }, passed: { type: 'boolean' }, details: { type: 'string' }, errors: { type: 'array', items: { type: 'string' } } } }
const verifies = await parallel([
  () => agent(`${FACTS}
VERIFY plumbing after grub-strip + rename. In ${FERN} run: bash -n conf.sh ; ./conf.sh --defconfig ; make configcheck ; make -n build ; make show-config. passed=true only if all exit 0 AND make -n build still links the kernel to fern.bin (capture the final ld line). Capture any error verbatim.`,
    { label: 'verify:plumbing', phase: 'Verify', schema: VER, agentType: 'general-purpose' }),
  () => agent(`${FACTS}
VERIFY contracts in ${FERN}: (1) grep -rin grub across build/ Makefile conf.sh .forestos_config -> expect ZERO functional grub refs (Grub/ dir gone); (2) no CONFIG_GRUB_TIMEOUT anywhere; (3) build/foreb.mk FOREBO_DIR resolves ${ROOT}/foreboots (print \`make -n forebo\` or echo of the var); (4) ENABLE_FOREB_BOOTLOADER default y; (5) symlinks fern/libs and fern/forestos-toolchain still resolve. List any violation as an error. passed=true only if clean.`,
    { label: 'verify:contracts', phase: 'Verify', schema: VER, agentType: 'general-purpose' }),
])
const failures = verifies.filter(Boolean).filter(v => !v.passed)
log(`Verify: ${verifies.filter(Boolean).length - failures.length}/${verifies.filter(Boolean).length} passed`)

return {
  surveys: surveys.filter(Boolean).map(s => s.domain),
  strips: strips.filter(Boolean).map(s => ({ owner: s.owner, files: s.files || [] })),
  rename: renameRes ? renameRes.summary : 'FAILED',
  verify: verifies.filter(Boolean).map(v => ({ check: v.check, passed: v.passed, errors: v.errors || [] })),
  failures: failures.length,
}
