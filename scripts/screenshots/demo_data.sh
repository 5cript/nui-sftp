#!/usr/bin/env bash
# Writes the neutral demo files for the screenshots: a home directory for the local side and a tree for the server.
# Usage: demo_data.sh <local home directory> <remote tree directory>
#
# The website project exists on both sides with known differences, so the synchronize dialog has rows in both
# directions:
#   upload:   css/main.css (local newer), contact.html (local newer), js/contact-form.js and
#             blog/2026-10-20-autumn-update.md (local only)
#   download: js/app.js and sitemap.xml (remote newer), images/team.jpg (remote only)
# Both directories are replaced. The website starts as one copy on both sides, so the unchanged files match byte for
# byte and in their modification times. Needs git, gzip, tar and ImageMagick (magick).
set -euo pipefail

localHome="${1:?local home directory}"
remoteTree="${2:?remote tree directory}"

export LC_ALL=C
export GIT_CONFIG_GLOBAL=/dev/null
export GIT_CONFIG_NOSYSTEM=1

writeFile() {
    local path="$1"
    local content="$2"
    local age="${3:-20 days ago}"
    mkdir -p -- "$(dirname -- "$path")"
    printf '%s\n' "$content" > "$path"
    touch -d "$age" -- "$path"
}

makeImage() {
    local path="$1"
    local size="$2"
    local gradient="$3"
    local age="${4:-20 days ago}"
    mkdir -p -- "$(dirname -- "$path")"
    magick -size "$size" "gradient:$gradient" -quality 85 "$path"
    touch -d "$age" -- "$path"
}

dayString() {
    date -d "$1 days ago" +%F
}

writeWebsite() {
    local root="$1"
    writeFile "$root/README.md" "# website

Static site of the Example Studio, built with plain HTML, CSS and a little JavaScript.

    npm install
    npm run build
    npm run deploy"
    writeFile "$root/package.json" '{
  "name": "website",
  "version": "1.4.1",
  "private": true,
  "scripts": {
    "build": "postcss css/main.css -o dist/main.css",
    "deploy": "rsync -a dist/ public/"
  },
  "devDependencies": {
    "autoprefixer": "^10.4.20",
    "postcss-cli": "^11.0.0"
  }
}'
    local page
    for page in index about 404; do
        writeFile "$root/$page.html" "<!doctype html>
<html lang=\"en\">
<head>
    <meta charset=\"utf-8\">
    <title>Example Studio - $page</title>
    <link rel=\"stylesheet\" href=\"css/main.css\">
</head>
<body>
    <header><img src=\"images/logo.svg\" alt=\"Example Studio\"></header>
    <main id=\"$page\"></main>
    <script src=\"js/app.js\"></script>
</body>
</html>"
    done
    writeFile "$root/contact.html" '<!doctype html>
<html lang="en">
<head><meta charset="utf-8"><title>Example Studio - contact</title></head>
<body><main id="contact"><a href="mailto:hello@example.com">hello@example.com</a></main></body>
</html>'
    writeFile "$root/robots.txt" "User-agent: *
Allow: /
Sitemap: https://www.example.com/sitemap.xml"
    writeFile "$root/sitemap.xml" '<?xml version="1.0" encoding="UTF-8"?>
<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">
  <url><loc>https://www.example.com/</loc></url>
  <url><loc>https://www.example.com/about.html</loc></url>
</urlset>'
    writeFile "$root/favicon.svg" '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16"><circle cx="8" cy="8" r="7" fill="#2a9d8f"/></svg>'
    writeFile "$root/css/main.css" ":root {
    --accent: #2a9d8f;
    --text: #1d3557;
}

body {
    margin: 0;
    font-family: system-ui, sans-serif;
    color: var(--text);
}"
    writeFile "$root/css/print.css" "@media print {
    header, footer { display: none; }
}"
    writeFile "$root/js/app.js" "document.addEventListener('DOMContentLoaded', () => {
    document.body.classList.add('ready');
});"
    writeFile "$root/images/logo.svg" '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 120 32"><text x="0" y="24" font-size="24">Example</text></svg>'
    makeImage "$root/images/hero.jpg" 1600x900 '#264653-#2a9d8f'
    makeImage "$root/images/office.jpg" 1200x800 '#e9c46a-#f4a261'
    writeFile "$root/blog/index.html" '<!doctype html><html lang="en"><head><meta charset="utf-8"><title>Blog</title></head><body></body></html>'
    writeFile "$root/blog/2026-08-14-hello-world.md" "# Hello, world

The new website is online."
    writeFile "$root/blog/2026-09-02-new-design.md" "# A new design

Lighter colors, bigger type and a faster start page."
}

writeLocalHome() {
    mkdir -p -- "$localHome"/{Desktop,Documents,Downloads,Music,Pictures,Videos,Projects}
    mkdir -p -- "$localHome/.config"
    cat > "$localHome/.config/user-dirs.dirs" <<'EOF'
XDG_DESKTOP_DIR="$HOME/Desktop"
XDG_DOCUMENTS_DIR="$HOME/Documents"
XDG_DOWNLOAD_DIR="$HOME/Downloads"
XDG_MUSIC_DIR="$HOME/Music"
XDG_PICTURES_DIR="$HOME/Pictures"
XDG_VIDEOS_DIR="$HOME/Videos"
EOF

    writeFile "$localHome/Documents/notes.md" "# Notes

- Renew the TLS certificate before the end of the month
- Move the nightly backup to 02:00"
    writeFile "$localHome/Documents/meeting-notes.md" "# Weekly meeting

1. Website release 1.5
2. New staging server"
    writeFile "$localHome/Documents/drafts/release-notes.md" "# Release 1.5

- Contact form with validation
- Autumn blog post" "2 hours ago"
    writeFile "$localHome/Documents/drafts/roadmap.md" "# Roadmap

| Quarter | Goal |
|---|---|
| Q4 | Online shop |
| Q1 | Newsletter |" "3 hours ago"
    makeImage "$localHome/Pictures/wallpaper.jpg" 1920x1080 '#1d3557-#457b9d'
    makeImage "$localHome/Pictures/holiday.jpg" 1200x800 '#f4a261-#e76f51'

    local website="$localHome/Projects/website"
    writeFile "$website/css/main.css" ":root {
    --accent: #e76f51;
    --text: #1d3557;
}

body {
    margin: 0;
    font-family: system-ui, sans-serif;
    color: var(--text);
}

.contact-form input:invalid {
    border-color: var(--accent);
}" "1 hour ago"
    writeFile "$website/contact.html" '<!doctype html>
<html lang="en">
<head><meta charset="utf-8"><title>Example Studio - contact</title></head>
<body><main id="contact"><form class="contact-form"><input type="email" required></form></main>
<script src="js/contact-form.js"></script></body>
</html>' "1 hour ago"
    writeFile "$website/js/contact-form.js" "document.querySelector('.contact-form').addEventListener('submit', (event) => {
    event.preventDefault();
});" "50 minutes ago"
    writeFile "$website/blog/2026-10-20-autumn-update.md" "# Autumn update

What changed on the website this autumn." "4 hours ago"

    writeApiServer "$localHome/Projects/api-server"

    writeFile "$localHome/Projects/dotfiles/bashrc" "alias ll='ls -lh'
export EDITOR=vim"
    writeFile "$localHome/Projects/dotfiles/vimrc" "set number
syntax on"
    writeFile "$localHome/Projects/dotfiles/install.sh" "#!/bin/sh
ln -sf \"\$PWD/bashrc\" ~/.bashrc"
}

commitAt() {
    local age="$1"
    local message="$2"
    local stamp
    stamp="$(date -d "$age" --iso-8601=seconds)"
    git add -A
    GIT_AUTHOR_DATE="$stamp" GIT_COMMITTER_DATE="$stamp" git commit -q --no-gpg-sign -m "$message"
}

writeApiServer() {
    local root="$1"
    mkdir -p -- "$root"
    (
        cd -- "$root"
        export GIT_AUTHOR_NAME="Demo User" GIT_AUTHOR_EMAIL="demo@example.com"
        export GIT_COMMITTER_NAME="Demo User" GIT_COMMITTER_EMAIL="demo@example.com"
        git init -q -b main
        writeFile README.md "# api-server

Small HTTP API for the Example Studio website."
        writeFile go.mod "module example.com/api-server

go 1.23"
        writeFile main.go 'package main

func main() {
	run()
}'
        commitAt "12 days ago" "Initial commit"
        writeFile handlers/health.go 'package handlers'
        commitAt "11 days ago" "Add a health check endpoint"
        writeFile handlers/users.go 'package handlers'
        commitAt "9 days ago" "List and create users"
        writeFile internal/db/db.go 'package db'
        commitAt "8 days ago" "Store users in PostgreSQL"
        writeFile Dockerfile "FROM golang:1.23 AS build
COPY . /src
RUN cd /src && go build -o /api-server

FROM gcr.io/distroless/base
COPY --from=build /api-server /api-server
ENTRYPOINT [\"/api-server\"]"
        commitAt "7 days ago" "Build a container image"
        git tag v0.3.0
        git switch -q -c feature/rate-limit
        writeFile internal/ratelimit/limiter.go 'package ratelimit'
        commitAt "5 days ago" "Add a token bucket rate limiter"
        writeFile handlers/users.go 'package handlers // rate limited'
        commitAt "4 days ago" "Rate limit user creation"
        git switch -q main
        writeFile Makefile "build:
	go build ./...

test:
	go test ./..."
        commitAt "3 days ago" "Add a Makefile"
        local stamp
        stamp="$(date -d "2 days ago" --iso-8601=seconds)"
        GIT_AUTHOR_DATE="$stamp" GIT_COMMITTER_DATE="$stamp" \
            git merge -q --no-ff --no-gpg-sign -m "Merge branch 'feature/rate-limit'" feature/rate-limit
        writeFile .gitignore "/api-server
*.log"
        commitAt "1 day ago" "Ignore build output"
        git tag v0.4.0
    )
}

writeAccessLog() {
    local path="$1"
    local index
    local addresses=(203.0.113.24 198.51.100.7 203.0.113.81 192.0.2.15 198.51.100.42)
    local paths=(/ /about.html /blog/ /blog/2026-09-02-new-design.md /contact.html /css/main.css /images/hero.jpg)
    mkdir -p -- "$(dirname -- "$path")"
    : > "$path"
    for index in $(seq 60 -1 1); do
        printf '%s - - [%s] "GET %s HTTP/1.1" 200 %d "-" "Mozilla/5.0"\n' \
            "${addresses[index % ${#addresses[@]}]}" \
            "$(date -d "$((index * 7)) minutes ago" '+%d/%b/%Y:%H:%M:%S %z')" \
            "${paths[index % ${#paths[@]}]}" \
            $((1200 + index * 37)) >> "$path"
    done
}

writeRemoteTree() {
    touch -- "$remoteTree/.nui-sftp-screenshots-demo"

    local website="$remoteTree/website"
    writeFile "$website/js/app.js" "document.addEventListener('DOMContentLoaded', () => {
    document.body.classList.add('ready');
    document.querySelectorAll('img').forEach((image) => image.loading = 'lazy');
});" "30 minutes ago"
    writeFile "$website/sitemap.xml" '<?xml version="1.0" encoding="UTF-8"?>
<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">
  <url><loc>https://www.example.com/</loc></url>
  <url><loc>https://www.example.com/about.html</loc></url>
  <url><loc>https://www.example.com/blog/</loc></url>
</urlset>' "6 hours ago"
    makeImage "$website/images/team.jpg" 1200x800 '#a8dadc-#457b9d' "5 hours ago"

    local day
    for day in 3 2 1; do
        local name
        name="db-$(dayString "$day").sql.gz"
        mkdir -p -- "$remoteTree/backups"
        printf -- '-- PostgreSQL dump of the website database, day %s\nCREATE TABLE posts (id serial, title text);\n' "$day" |
            gzip -n > "$remoteTree/backups/$name"
        touch -d "$day days ago 02:00" -- "$remoteTree/backups/$name"
    done

    writeAccessLog "$remoteTree/logs/access.log"
    writeFile "$remoteTree/logs/error.log" "[warn] client 198.51.100.7 closed the connection while reading the response
[error] open() \"/srv/www/favicon.ico\" failed (2: No such file or directory)" "40 minutes ago"

    mkdir -p -- "$remoteTree/releases"
    tar -C "$website" --mtime="10 days ago" -czf "$remoteTree/releases/website-1.4.0.tar.gz" .
    touch -d "10 days ago" -- "$remoteTree/releases/website-1.4.0.tar.gz"
    tar -C "$website" --mtime="3 days ago" -czf "$remoteTree/releases/website-1.4.1.tar.gz" .
    touch -d "3 days ago" -- "$remoteTree/releases/website-1.4.1.tar.gz"

    mkdir -p -- "$remoteTree/docs"
    writeFile "$remoteTree/docker-compose.yml" "services:
  web:
    image: nginx:1.27
    volumes:
      - ./website:/usr/share/nginx/html:ro
    ports:
      - \"8080:80\""
    writeFile "$remoteTree/notes.md" "# Server notes

- Backups run every night at 02:00
- Logs rotate weekly"
}

rm -rf -- "$localHome" "$remoteTree"
mkdir -p -- "$localHome/Projects" "$remoteTree"
writeWebsite "$localHome/Projects/website"
cp -a -- "$localHome/Projects/website" "$remoteTree/website"
writeLocalHome
writeRemoteTree
printf 'demo home: %s\ndemo server tree: %s\n' "$localHome" "$remoteTree"
