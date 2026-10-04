# zsh and fish for the shell integration tests; bash comes from the official bash images.
# Pinned so a test run is reproducible.
FROM alpine:3.24
RUN apk add --no-cache zsh fish
