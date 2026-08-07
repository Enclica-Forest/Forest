#!/usr/bin/env bash
set -euo pipefail

theme_root="initrd/usr/share/images/icons/hicolor"

mkdir -p \
    "${theme_root}/scalable/apps" \
    "${theme_root}/scalable/actions" \
    "${theme_root}/scalable/devices" \
    "${theme_root}/scalable/filesystems" \
    "${theme_root}/scalable/mimetypes"

cat > "${theme_root}/index.theme" <<'EOF'
[Icon Theme]
Name=hicolor
Comment=Forest OS Freedesktop fallback icon theme
Hidden=true
Directories=scalable/apps,scalable/actions,scalable/devices,scalable/filesystems,scalable/mimetypes

[scalable/apps]
Size=48
Type=Scalable
MinSize=16
MaxSize=256
Context=Applications

[scalable/actions]
Size=48
Type=Scalable
MinSize=16
MaxSize=256
Context=Actions

[scalable/devices]
Size=48
Type=Scalable
MinSize=16
MaxSize=256
Context=Devices

[scalable/filesystems]
Size=48
Type=Scalable
MinSize=16
MaxSize=256
Context=FileSystems

[scalable/mimetypes]
Size=48
Type=Scalable
MinSize=16
MaxSize=256
Context=MimeTypes
EOF

render_icon() {
    local out_file="$1"
    local bg="$2"
    local fg="$3"
    local symbol="$4"

    cat > "${out_file}" <<EOF
<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64" viewBox="0 0 64 64">
  <rect x="4" y="4" width="56" height="56" rx="12" fill="${bg}"/>
  <rect x="4.75" y="4.75" width="54.5" height="54.5" rx="11.25" fill="none" stroke="#00000024" stroke-width="1.5"/>
EOF

    case "${symbol}" in
        page_plus)
            cat >> "${out_file}" <<EOF
  <rect x="20" y="13" width="24" height="36" rx="3" fill="none" stroke="${fg}" stroke-width="3"/>
  <line x1="32" y1="20" x2="32" y2="36" stroke="${fg}" stroke-width="3" stroke-linecap="round"/>
  <line x1="24" y1="28" x2="40" y2="28" stroke="${fg}" stroke-width="3" stroke-linecap="round"/>
EOF
            ;;
        folder_open)
            cat >> "${out_file}" <<EOF
  <path d="M10 22h18l4 5h22a4 4 0 0 1 4 4v14a5 5 0 0 1-5 5H13a5 5 0 0 1-5-5V26a4 4 0 0 1 2-4z" fill="none" stroke="${fg}" stroke-width="3" stroke-linejoin="round"/>
  <path d="M11 31h46" stroke="${fg}" stroke-width="3" stroke-linecap="round"/>
EOF
            ;;
        floppy)
            cat >> "${out_file}" <<EOF
  <rect x="15" y="12" width="34" height="40" rx="4" fill="none" stroke="${fg}" stroke-width="3"/>
  <rect x="21" y="18" width="22" height="10" rx="2" fill="none" stroke="${fg}" stroke-width="3"/>
  <rect x="24" y="36" width="16" height="10" rx="2" fill="none" stroke="${fg}" stroke-width="3"/>
EOF
            ;;
        copy)
            cat >> "${out_file}" <<EOF
  <rect x="16" y="20" width="24" height="28" rx="3" fill="none" stroke="${fg}" stroke-width="3"/>
  <rect x="24" y="14" width="24" height="28" rx="3" fill="none" stroke="${fg}" stroke-width="3"/>
EOF
            ;;
        cut)
            cat >> "${out_file}" <<EOF
  <circle cx="22" cy="44" r="5" fill="none" stroke="${fg}" stroke-width="3"/>
  <circle cx="42" cy="44" r="5" fill="none" stroke="${fg}" stroke-width="3"/>
  <line x1="18" y1="16" x2="44" y2="40" stroke="${fg}" stroke-width="3" stroke-linecap="round"/>
  <line x1="46" y1="16" x2="20" y2="40" stroke="${fg}" stroke-width="3" stroke-linecap="round"/>
EOF
            ;;
        clipboard)
            cat >> "${out_file}" <<EOF
  <rect x="18" y="16" width="28" height="34" rx="4" fill="none" stroke="${fg}" stroke-width="3"/>
  <rect x="24" y="10" width="16" height="8" rx="3" fill="none" stroke="${fg}" stroke-width="3"/>
  <line x1="24" y1="28" x2="40" y2="28" stroke="${fg}" stroke-width="3" stroke-linecap="round"/>
  <line x1="24" y1="36" x2="36" y2="36" stroke="${fg}" stroke-width="3" stroke-linecap="round"/>
EOF
            ;;
        trash)
            cat >> "${out_file}" <<EOF
  <rect x="20" y="20" width="24" height="30" rx="3" fill="none" stroke="${fg}" stroke-width="3"/>
  <path d="M17 20h30M24 16h16" stroke="${fg}" stroke-width="3" stroke-linecap="round"/>
  <line x1="27" y1="27" x2="27" y2="44" stroke="${fg}" stroke-width="3" stroke-linecap="round"/>
  <line x1="37" y1="27" x2="37" y2="44" stroke="${fg}" stroke-width="3" stroke-linecap="round"/>
EOF
            ;;
        home)
            cat >> "${out_file}" <<EOF
  <path d="M12 32l20-16 20 16" fill="none" stroke="${fg}" stroke-width="3.5" stroke-linecap="round" stroke-linejoin="round"/>
  <path d="M18 31v19h28V31" fill="none" stroke="${fg}" stroke-width="3.5" stroke-linejoin="round"/>
  <rect x="28" y="38" width="8" height="12" rx="1" fill="none" stroke="${fg}" stroke-width="3"/>
EOF
            ;;
        search)
            cat >> "${out_file}" <<EOF
  <circle cx="28" cy="28" r="12" fill="none" stroke="${fg}" stroke-width="4"/>
  <line x1="37" y1="37" x2="49" y2="49" stroke="${fg}" stroke-width="4" stroke-linecap="round"/>
EOF
            ;;
        refresh)
            cat >> "${out_file}" <<EOF
  <path d="M46 22a14 14 0 1 0 3 14" fill="none" stroke="${fg}" stroke-width="3.5" stroke-linecap="round"/>
  <path d="M45 14v10h10" fill="none" stroke="${fg}" stroke-width="3.5" stroke-linecap="round" stroke-linejoin="round"/>
EOF
            ;;
        monitor)
            cat >> "${out_file}" <<EOF
  <rect x="13" y="15" width="38" height="26" rx="3" fill="none" stroke="${fg}" stroke-width="3"/>
  <line x1="24" y1="49" x2="40" y2="49" stroke="${fg}" stroke-width="3" stroke-linecap="round"/>
  <line x1="32" y1="41" x2="32" y2="49" stroke="${fg}" stroke-width="3" stroke-linecap="round"/>
EOF
            ;;
        harddisk)
            cat >> "${out_file}" <<EOF
  <rect x="13" y="18" width="38" height="28" rx="8" fill="none" stroke="${fg}" stroke-width="3"/>
  <circle cx="32" cy="32" r="7" fill="none" stroke="${fg}" stroke-width="3"/>
  <circle cx="44" cy="32" r="2.5" fill="${fg}"/>
EOF
            ;;
        optical)
            cat >> "${out_file}" <<EOF
  <circle cx="32" cy="32" r="17" fill="none" stroke="${fg}" stroke-width="3"/>
  <circle cx="32" cy="32" r="4" fill="none" stroke="${fg}" stroke-width="3"/>
  <line x1="32" y1="15" x2="39" y2="26" stroke="${fg}" stroke-width="3" stroke-linecap="round"/>
EOF
            ;;
        mouse)
            cat >> "${out_file}" <<EOF
  <rect x="22" y="12" width="20" height="40" rx="10" fill="none" stroke="${fg}" stroke-width="3"/>
  <line x1="32" y1="14" x2="32" y2="25" stroke="${fg}" stroke-width="3" stroke-linecap="round"/>
EOF
            ;;
        keyboard)
            cat >> "${out_file}" <<EOF
  <rect x="11" y="20" width="42" height="24" rx="4" fill="none" stroke="${fg}" stroke-width="3"/>
  <path d="M17 27h30M17 33h30M17 39h20" fill="none" stroke="${fg}" stroke-width="2.4" stroke-linecap="round"/>
EOF
            ;;
        printer)
            cat >> "${out_file}" <<EOF
  <rect x="18" y="12" width="28" height="12" rx="2" fill="none" stroke="${fg}" stroke-width="3"/>
  <rect x="14" y="24" width="36" height="18" rx="4" fill="none" stroke="${fg}" stroke-width="3"/>
  <rect x="20" y="38" width="24" height="14" rx="2" fill="none" stroke="${fg}" stroke-width="3"/>
EOF
            ;;
        camera)
            cat >> "${out_file}" <<EOF
  <rect x="12" y="20" width="40" height="26" rx="5" fill="none" stroke="${fg}" stroke-width="3"/>
  <circle cx="32" cy="33" r="8" fill="none" stroke="${fg}" stroke-width="3"/>
  <rect x="20" y="15" width="12" height="6" rx="2" fill="none" stroke="${fg}" stroke-width="3"/>
EOF
            ;;
        headphones)
            cat >> "${out_file}" <<EOF
  <path d="M18 32a14 14 0 0 1 28 0" fill="none" stroke="${fg}" stroke-width="3.5" stroke-linecap="round"/>
  <rect x="14" y="32" width="8" height="14" rx="3" fill="none" stroke="${fg}" stroke-width="3"/>
  <rect x="42" y="32" width="8" height="14" rx="3" fill="none" stroke="${fg}" stroke-width="3"/>
EOF
            ;;
        phone)
            cat >> "${out_file}" <<EOF
  <rect x="22" y="10" width="20" height="44" rx="4" fill="none" stroke="${fg}" stroke-width="3"/>
  <circle cx="32" cy="47" r="2.2" fill="${fg}"/>
  <line x1="27" y1="16" x2="37" y2="16" stroke="${fg}" stroke-width="2.5" stroke-linecap="round"/>
EOF
            ;;
        folder)
            cat >> "${out_file}" <<EOF
  <path d="M9 23h18l4 5h24v18a5 5 0 0 1-5 5H14a5 5 0 0 1-5-5V27a4 4 0 0 1 0-4z" fill="none" stroke="${fg}" stroke-width="3" stroke-linejoin="round"/>
EOF
            ;;
        folder_doc)
            cat >> "${out_file}" <<EOF
  <path d="M9 23h18l4 5h24v18a5 5 0 0 1-5 5H14a5 5 0 0 1-5-5V27a4 4 0 0 1 0-4z" fill="none" stroke="${fg}" stroke-width="3" stroke-linejoin="round"/>
  <rect x="26" y="31" width="14" height="16" rx="2" fill="none" stroke="${fg}" stroke-width="2.5"/>
EOF
            ;;
        folder_down)
            cat >> "${out_file}" <<EOF
  <path d="M9 23h18l4 5h24v18a5 5 0 0 1-5 5H14a5 5 0 0 1-5-5V27a4 4 0 0 1 0-4z" fill="none" stroke="${fg}" stroke-width="3" stroke-linejoin="round"/>
  <line x1="32" y1="32" x2="32" y2="44" stroke="${fg}" stroke-width="3" stroke-linecap="round"/>
  <path d="M26 39l6 6 6-6" fill="none" stroke="${fg}" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"/>
EOF
            ;;
        folder_music)
            cat >> "${out_file}" <<EOF
  <path d="M9 23h18l4 5h24v18a5 5 0 0 1-5 5H14a5 5 0 0 1-5-5V27a4 4 0 0 1 0-4z" fill="none" stroke="${fg}" stroke-width="3" stroke-linejoin="round"/>
  <path d="M37 31v11a3 3 0 1 1-3-3" fill="none" stroke="${fg}" stroke-width="2.6" stroke-linecap="round" stroke-linejoin="round"/>
EOF
            ;;
        folder_pic)
            cat >> "${out_file}" <<EOF
  <path d="M9 23h18l4 5h24v18a5 5 0 0 1-5 5H14a5 5 0 0 1-5-5V27a4 4 0 0 1 0-4z" fill="none" stroke="${fg}" stroke-width="3" stroke-linejoin="round"/>
  <circle cx="23" cy="34" r="2.5" fill="${fg}"/>
  <path d="M20 45l7-7 5 5 6-6 6 8" fill="none" stroke="${fg}" stroke-width="2.6" stroke-linecap="round" stroke-linejoin="round"/>
EOF
            ;;
        folder_video)
            cat >> "${out_file}" <<EOF
  <path d="M9 23h18l4 5h24v18a5 5 0 0 1-5 5H14a5 5 0 0 1-5-5V27a4 4 0 0 1 0-4z" fill="none" stroke="${fg}" stroke-width="3" stroke-linejoin="round"/>
  <path d="M28 33l10 6-10 6z" fill="none" stroke="${fg}" stroke-width="2.8" stroke-linejoin="round"/>
EOF
            ;;
        home_user)
            cat >> "${out_file}" <<EOF
  <path d="M12 32l20-16 20 16" fill="none" stroke="${fg}" stroke-width="3.5" stroke-linecap="round" stroke-linejoin="round"/>
  <path d="M18 31v19h28V31" fill="none" stroke="${fg}" stroke-width="3.5" stroke-linejoin="round"/>
  <circle cx="32" cy="41" r="4" fill="none" stroke="${fg}" stroke-width="2.6"/>
EOF
            ;;
        server)
            cat >> "${out_file}" <<EOF
  <rect x="14" y="13" width="36" height="12" rx="3" fill="none" stroke="${fg}" stroke-width="3"/>
  <rect x="14" y="27" width="36" height="12" rx="3" fill="none" stroke="${fg}" stroke-width="3"/>
  <rect x="14" y="41" width="36" height="10" rx="3" fill="none" stroke="${fg}" stroke-width="3"/>
  <circle cx="20" cy="19" r="1.8" fill="${fg}"/>
  <circle cx="20" cy="33" r="1.8" fill="${fg}"/>
  <circle cx="20" cy="46" r="1.8" fill="${fg}"/>
EOF
            ;;
        usb)
            cat >> "${out_file}" <<EOF
  <rect x="23" y="12" width="18" height="40" rx="4" fill="none" stroke="${fg}" stroke-width="3"/>
  <rect x="26" y="12" width="12" height="7" rx="1.5" fill="none" stroke="${fg}" stroke-width="3"/>
  <line x1="28" y1="25" x2="36" y2="25" stroke="${fg}" stroke-width="2.5" stroke-linecap="round"/>
  <line x1="28" y1="32" x2="36" y2="32" stroke="${fg}" stroke-width="2.5" stroke-linecap="round"/>
EOF
            ;;
        text)
            cat >> "${out_file}" <<EOF
  <rect x="18" y="12" width="28" height="40" rx="3" fill="none" stroke="${fg}" stroke-width="3"/>
  <line x1="24" y1="24" x2="40" y2="24" stroke="${fg}" stroke-width="2.8" stroke-linecap="round"/>
  <line x1="24" y1="31" x2="40" y2="31" stroke="${fg}" stroke-width="2.8" stroke-linecap="round"/>
  <line x1="24" y1="38" x2="36" y2="38" stroke="${fg}" stroke-width="2.8" stroke-linecap="round"/>
EOF
            ;;
        html)
            cat >> "${out_file}" <<EOF
  <rect x="17" y="12" width="30" height="40" rx="3" fill="none" stroke="${fg}" stroke-width="3"/>
  <path d="M24 31l-5 5 5 5M40 31l5 5-5 5M31 44l2-16" fill="none" stroke="${fg}" stroke-width="2.8" stroke-linecap="round" stroke-linejoin="round"/>
EOF
            ;;
        pdf)
            cat >> "${out_file}" <<EOF
  <rect x="17" y="12" width="30" height="40" rx="3" fill="none" stroke="${fg}" stroke-width="3"/>
  <path d="M24 42c3-8 7-12 12-16 1 6 3 10 8 14-8 2-14 2-20 2z" fill="none" stroke="${fg}" stroke-width="2.5" stroke-linejoin="round"/>
EOF
            ;;
        zip)
            cat >> "${out_file}" <<EOF
  <rect x="17" y="12" width="30" height="40" rx="3" fill="none" stroke="${fg}" stroke-width="3"/>
  <line x1="32" y1="18" x2="32" y2="46" stroke="${fg}" stroke-width="3" stroke-dasharray="3 3" stroke-linecap="round"/>
EOF
            ;;
        image)
            cat >> "${out_file}" <<EOF
  <rect x="14" y="16" width="36" height="32" rx="3" fill="none" stroke="${fg}" stroke-width="3"/>
  <circle cx="24" cy="26" r="3" fill="${fg}"/>
  <path d="M18 42l8-9 7 7 6-6 7 8" fill="none" stroke="${fg}" stroke-width="2.8" stroke-linecap="round" stroke-linejoin="round"/>
EOF
            ;;
        audio)
            cat >> "${out_file}" <<EOF
  <path d="M20 38h7l9 8V18l-9 8h-7z" fill="none" stroke="${fg}" stroke-width="3" stroke-linejoin="round"/>
  <path d="M41 27c3 2 3 8 0 10M45 23c5 4 5 14 0 18" fill="none" stroke="${fg}" stroke-width="2.8" stroke-linecap="round"/>
EOF
            ;;
        video)
            cat >> "${out_file}" <<EOF
  <rect x="14" y="18" width="36" height="28" rx="4" fill="none" stroke="${fg}" stroke-width="3"/>
  <path d="M28 27l11 5-11 5z" fill="none" stroke="${fg}" stroke-width="2.8" stroke-linejoin="round"/>
EOF
            ;;
        gear)
            cat >> "${out_file}" <<EOF
  <circle cx="32" cy="32" r="7" fill="none" stroke="${fg}" stroke-width="3"/>
  <path d="M32 19v4M32 41v4M19 32h4M41 32h4M23 23l3 3M38 38l3 3M41 23l-3 3M23 41l3-3" fill="none" stroke="${fg}" stroke-width="2.8" stroke-linecap="round"/>
EOF
            ;;
        json)
            cat >> "${out_file}" <<EOF
  <rect x="17" y="12" width="30" height="40" rx="3" fill="none" stroke="${fg}" stroke-width="3"/>
  <path d="M27 24c-3 0-3 4-3 4s0 4-3 4c3 0 3 4 3 4s0 4 3 4M37 24c3 0 3 4 3 4s0 4 3 4c-3 0-3 4-3 4s0 4-3 4" fill="none" stroke="${fg}" stroke-width="2.5" stroke-linecap="round"/>
EOF
            ;;
        script)
            cat >> "${out_file}" <<EOF
  <rect x="17" y="12" width="30" height="40" rx="3" fill="none" stroke="${fg}" stroke-width="3"/>
  <path d="M23 30h8M23 36h14" fill="none" stroke="${fg}" stroke-width="2.8" stroke-linecap="round"/>
  <path d="M22 24l-3 3 3 3M42 42l3-3-3-3" fill="none" stroke="${fg}" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"/>
EOF
            ;;
        *)
            echo "Unknown symbol: ${symbol}" >&2
            return 1
            ;;
    esac

    cat >> "${out_file}" <<'EOF'
</svg>
EOF
}

create_icon_data() {
    local out_file="$1"
    local display_name="$2"
    cat > "${out_file}" <<EOF
[Icon Data]
DisplayName=${display_name}
EmbeddedTextRectangle=120,140,880,900
AttachPoints=200,220|800,220|500,500|200,850|800,850
EOF
}

entries=(
  "actions|document-new|#0C7E5D|#F8FFF9|page_plus"
  "actions|document-open|#0F766E|#F2FFFE|folder_open"
  "actions|document-save|#1D4ED8|#F5F9FF|floppy"
  "actions|document-print|#374151|#F9FAFB|printer"
  "actions|document-properties|#1F2937|#F9FAFB|gear"
  "actions|document-revert|#0284C7|#EFFAFF|refresh"
  "actions|document-send|#0E7490|#ECFEFF|folder_open"
  "actions|edit-copy|#2563EB|#F6FAFF|copy"
  "actions|edit-cut|#0E7490|#F1FCFF|cut"
  "actions|edit-paste|#0D9488|#F1FFFD|clipboard"
  "actions|edit-delete|#B91C1C|#FFF6F6|trash"
  "actions|edit-find|#1E40AF|#F4F8FF|search"
  "actions|edit-find-replace|#0F766E|#F0FDFA|search"
  "actions|edit-redo|#0369A1|#F0F9FF|refresh"
  "actions|edit-undo|#0EA5E9|#F0F9FF|refresh"
  "actions|list-add|#15803D|#F0FDF4|page_plus"
  "actions|list-remove|#B91C1C|#FEF2F2|trash"
  "actions|media-playback-start|#1D4ED8|#EFF6FF|video"
  "actions|media-playback-pause|#4338CA|#EEF2FF|video"
  "actions|process-stop|#991B1B|#FEF2F2|trash"
  "actions|go-home|#166534|#F5FFF4|home"
  "actions|system-search|#1E40AF|#F4F8FF|search"
  "actions|view-refresh|#0891B2|#F2FCFF|refresh"
  "devices|computer|#334155|#F8FAFC|monitor"
  "devices|drive-harddisk|#475569|#F8FAFC|harddisk"
  "devices|drive-harddisk-solidstate|#334155|#F1F5F9|harddisk"
  "devices|drive-optical|#0F766E|#F0FDFA|optical"
  "devices|drive-removable-media-usb|#0F766E|#ECFDF5|usb"
  "devices|media-floppy|#1D4ED8|#EFF6FF|floppy"
  "devices|audio-card|#6D28D9|#F5F3FF|audio"
  "devices|battery|#166534|#F0FDF4|phone"
  "devices|battery-caution|#B45309|#FFFBEB|phone"
  "devices|battery-low|#B91C1C|#FEF2F2|phone"
  "devices|bluetooth|#1E40AF|#EFF6FF|usb"
  "devices|input-mouse|#4338CA|#EEF2FF|mouse"
  "devices|input-keyboard|#0F172A|#F8FAFC|keyboard"
  "devices|input-tablet|#312E81|#EEF2FF|phone"
  "devices|microphone-sensitivity-high|#7C3AED|#F5F3FF|audio"
  "devices|multimedia-player|#1E3A8A|#EEF2FF|monitor"
  "devices|network-wired|#4B5563|#F9FAFB|server"
  "devices|network-wireless|#0369A1|#F0F9FF|server"
  "devices|printer|#374151|#F9FAFB|printer"
  "devices|scanner|#0F172A|#F8FAFC|printer"
  "devices|camera-photo|#0E7490|#ECFEFF|camera"
  "devices|audio-headphones|#4F46E5|#EEF2FF|headphones"
  "devices|phone|#312E81|#EEF2FF|phone"
  "filesystems|folder|#B45309|#FFF7ED|folder"
  "filesystems|folder-archive|#92400E|#FFFBEB|folder_down"
  "filesystems|folder-development|#1D4ED8|#EFF6FF|folder_doc"
  "filesystems|folder-documents|#A16207|#FFFBEB|folder_doc"
  "filesystems|folder-download|#CA8A04|#FEFCE8|folder_down"
  "filesystems|folder-music|#9333EA|#FAF5FF|folder_music"
  "filesystems|folder-pictures|#0E7490|#ECFEFF|folder_pic"
  "filesystems|folder-publicshare|#0F766E|#F0FDFA|folder_open"
  "filesystems|folder-recent|#0EA5E9|#F0F9FF|folder_doc"
  "filesystems|folder-remote|#2563EB|#EFF6FF|folder_open"
  "filesystems|folder-saved-search|#1E40AF|#EFF6FF|folder_doc"
  "filesystems|folder-templates|#CA8A04|#FFFBEB|folder_doc"
  "filesystems|folder-videos|#2563EB|#EFF6FF|folder_video"
  "filesystems|folder-visiting|#0E7490|#ECFEFF|folder_open"
  "filesystems|network-workgroup|#4B5563|#F9FAFB|server"
  "filesystems|start-here|#15803D|#F0FDF4|home"
  "filesystems|user-trash|#B91C1C|#FEF2F2|trash"
  "filesystems|user-trash-full|#7F1D1D|#FEF2F2|trash"
  "filesystems|user-home|#0F766E|#F0FDFA|home_user"
  "filesystems|user-desktop|#1E3A8A|#EEF2FF|monitor"
  "filesystems|network-server|#4B5563|#F9FAFB|server"
  "filesystems|drive-removable-media|#0F766E|#ECFDF5|usb"
  "mimetypes|application-xml|#0F766E|#ECFDF5|html"
  "mimetypes|application-yaml|#0284C7|#F0F9FF|json"
  "mimetypes|application-x-c|#1D4ED8|#EFF6FF|script"
  "mimetypes|application-x-cplusplus|#1E40AF|#EFF6FF|script"
  "mimetypes|application-x-shellscript|#111827|#F9FAFB|script"
  "mimetypes|application-x-sharedlib|#374151|#F9FAFB|gear"
  "mimetypes|application-x-iso9660-image|#0F766E|#F0FDFA|optical"
  "mimetypes|font-x-generic|#4B5563|#F9FAFB|text"
  "mimetypes|text-plain|#4B5563|#F9FAFB|text"
  "mimetypes|text-csv|#0E7490|#ECFEFF|text"
  "mimetypes|text-html|#EA580C|#FFF7ED|html"
  "mimetypes|text-markdown|#1F2937|#F9FAFB|text"
  "mimetypes|text-x-readme|#166534|#F0FDF4|text"
  "mimetypes|text-x-log|#7C2D12|#FFF7ED|text"
  "mimetypes|application-pdf|#B91C1C|#FEF2F2|pdf"
  "mimetypes|application-zip|#0F766E|#ECFDF5|zip"
  "mimetypes|image-x-generic|#0E7490|#ECFEFF|image"
  "mimetypes|audio-x-generic|#7C3AED|#F5F3FF|audio"
  "mimetypes|video-x-generic|#1D4ED8|#EFF6FF|video"
  "mimetypes|application-x-executable|#374151|#F9FAFB|gear"
  "mimetypes|application-json|#2563EB|#EFF6FF|json"
  "mimetypes|text-x-script|#111827|#F9FAFB|script"
)

for entry in "${entries[@]}"; do
    IFS='|' read -r context name bg fg symbol <<< "${entry}"
    out_file="${theme_root}/scalable/${context}/${name}.svg"
    render_icon "${out_file}" "${bg}" "${fg}" "${symbol}"
done

app_names=(
  "utilities-terminal"
  "internet-web-browser"
  "system-file-manager"
  "preferences-system"
  "accessories-calculator"
  "office-calendar"
  "alarm-clock"
  "camera-web"
  "internet-chat"
  "internet-mail"
  "multimedia-music-player"
  "multimedia-video-player"
  "multimedia-photo-manager"
  "accessories-text-editor"
  "accessories-notes"
  "accessories-todo"
  "weather-app"
  "software-store"
  "system-software-install"
  "utilities-system-monitor"
  "applications-development"
  "utilities-archive-manager"
  "utilities-disk-utility"
  "utilities-partition-editor"
  "preferences-desktop-font"
  "image-viewer"
  "media-player"
  "screen-recorder"
  "screenshot-tool"
  "remote-desktop"
  "internet-ssh"
  "internet-ftp"
  "system-package-manager"
  "backup-manager"
  "restore-manager"
  "log-viewer"
  "help-browser"
  "help-about"
  "network-manager"
  "network-wireless-manager"
  "bluetooth-manager"
  "power-manager"
  "display-settings"
  "sound-settings"
  "keyboard-settings"
  "mouse-settings"
  "printer-manager"
  "user-manager"
  "security-center"
  "firewall-config"
  "software-update"
  "installer"
  "uninstaller"
  "task-manager"
  "file-search"
  "maps"
  "contacts"
  "calendar-agenda"
  "rss-reader"
  "podcast-player"
  "news-reader"
  "document-viewer"
  "pdf-viewer"
  "presentation-editor"
  "spreadsheet-editor"
  "word-processor"
  "database-client"
  "vm-manager"
  "container-manager"
  "terminal-multiplexer"
)

app_symbols=(
  "monitor"
  "gear"
  "folder_open"
  "search"
  "refresh"
  "home"
  "printer"
  "camera"
  "headphones"
  "phone"
  "text"
  "html"
  "pdf"
  "zip"
  "image"
  "audio"
  "video"
  "script"
  "json"
  "harddisk"
  "optical"
  "server"
  "clipboard"
  "copy"
  "page_plus"
)

app_bgs=(
  "#0F766E"
  "#1D4ED8"
  "#2563EB"
  "#0E7490"
  "#4338CA"
  "#4F46E5"
  "#1E40AF"
  "#0369A1"
  "#374151"
  "#475569"
  "#B45309"
  "#CA8A04"
  "#166534"
  "#15803D"
)

app_fgs=(
  "#F8FAFC"
  "#F0F9FF"
  "#EFF6FF"
  "#ECFEFF"
  "#EEF2FF"
  "#F0FDFA"
  "#FFFBEB"
)

for i in "${!app_names[@]}"; do
    name="${app_names[$i]}"
    symbol="${app_symbols[$((i % ${#app_symbols[@]}))]}"
    bg="${app_bgs[$((i % ${#app_bgs[@]}))]}"
    fg="${app_fgs[$((i % ${#app_fgs[@]}))]}"
    out_file="${theme_root}/scalable/apps/${name}.svg"
    render_icon "${out_file}" "${bg}" "${fg}" "${symbol}"
done

create_icon_data "${theme_root}/scalable/mimetypes/text-plain.icon" "Plain Text"
create_icon_data "${theme_root}/scalable/mimetypes/text-html.icon" "HTML Document"
create_icon_data "${theme_root}/scalable/mimetypes/application-pdf.icon" "PDF Document"
create_icon_data "${theme_root}/scalable/mimetypes/application-zip.icon" "ZIP Archive"
create_icon_data "${theme_root}/scalable/mimetypes/image-x-generic.icon" "Image File"
create_icon_data "${theme_root}/scalable/mimetypes/audio-x-generic.icon" "Audio File"
create_icon_data "${theme_root}/scalable/mimetypes/video-x-generic.icon" "Video File"
create_icon_data "${theme_root}/scalable/mimetypes/application-x-executable.icon" "Executable File"
create_icon_data "${theme_root}/scalable/mimetypes/application-json.icon" "JSON Document"
create_icon_data "${theme_root}/scalable/mimetypes/text-x-script.icon" "Script File"
create_icon_data "${theme_root}/scalable/mimetypes/application-xml.icon" "XML Document"
create_icon_data "${theme_root}/scalable/mimetypes/application-yaml.icon" "YAML Document"
create_icon_data "${theme_root}/scalable/mimetypes/application-x-c.icon" "C Source File"
create_icon_data "${theme_root}/scalable/mimetypes/application-x-cplusplus.icon" "C++ Source File"
create_icon_data "${theme_root}/scalable/mimetypes/application-x-shellscript.icon" "Shell Script"
create_icon_data "${theme_root}/scalable/mimetypes/application-x-sharedlib.icon" "Shared Library"
create_icon_data "${theme_root}/scalable/mimetypes/application-x-iso9660-image.icon" "ISO Image"
create_icon_data "${theme_root}/scalable/mimetypes/font-x-generic.icon" "Font File"
create_icon_data "${theme_root}/scalable/mimetypes/text-csv.icon" "CSV Document"
create_icon_data "${theme_root}/scalable/mimetypes/text-markdown.icon" "Markdown Document"
create_icon_data "${theme_root}/scalable/mimetypes/text-x-readme.icon" "Readme Document"
create_icon_data "${theme_root}/scalable/mimetypes/text-x-log.icon" "Log File"

total_icons=$(( ${#entries[@]} + ${#app_names[@]} ))
echo "Generated ${total_icons} icons in ${theme_root} (${#app_names[@]} apps)"
