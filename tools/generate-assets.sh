#!/bin/sh
# Emit the tracked xxd-compatible format using tools also present in the SDK image.
set -eu
OUT="${1:?usage: sh tools/generate-assets.sh output-directory}"
mkdir -p "$OUT"
for asset in web icon; do
    case "$asset" in
        web) input=web/index.html; file=web_page.c; symbol=web_page_html ;;
        icon) input=assets/icon0.png; file=icon_data.c; symbol=icon_png_data ;;
    esac
    {
        printf '/* Generated from %s by make. */\n' "$input"
        printf 'const unsigned char %s[] = {\n' "$symbol"
        od -An -v -t u1 "$input" | awk '{
            for (i = 1; i <= NF; i++) {
                if (n) printf ",%s", (n % 12 == 0 ? "\n  " : " ");
                else printf "  ";
                printf "0x%02x", $i;
                n++;
            }
        } END { if (n) printf "\n"; }'
        printf '};\nconst unsigned int %s_len = sizeof %s;\n' "$symbol" "$symbol"
    } > "$OUT/$file"
done
