#!/usr/bin/env bash
# Shared helpers for eval/*.sh — resolve a local musly binary or the Docker image.
#
# Prefer (in order):
#   1. MUSLY=/path/to/musly when executable
#   2. $ROOT/build/musly/musly
#   3. musly on PATH
#   4. Docker image MUSLY_IMAGE (default: musly:dev)
#
# When using Docker, call: musly_run <host_cwd> -- [musly args...]
# Mounts the FMA data directory (parent of TREE by default) so relative
# symlinks from tree/ → fma_small/ resolve inside the container.

: "${MUSLY_IMAGE:=musly:dev}"

sha256_file() {
  local f="$1"
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$f" | awk '{print $1}'
  else
    shasum -a 256 "$f" | awk '{print $1}'
  fi
}

resolve_musly() {
  MUSLY_MODE=""
  if [[ -n "${MUSLY:-}" && -x "$MUSLY" ]]; then
    MUSLY_MODE=local
    return 0
  fi
  if [[ -n "${MUSLY:-}" && "$MUSLY" != *"/build/musly/musly" ]]; then
    echo "MUSLY=$MUSLY is not executable" >&2
    exit 1
  fi
  if [[ -x "${ROOT}/build/musly/musly" ]]; then
    MUSLY="${ROOT}/build/musly/musly"
    MUSLY_MODE=local
    return 0
  fi
  if command -v musly >/dev/null 2>&1; then
    MUSLY="$(command -v musly)"
    MUSLY_MODE=local
    return 0
  fi
  if command -v docker >/dev/null 2>&1 \
      && docker image inspect "$MUSLY_IMAGE" >/dev/null 2>&1; then
    MUSLY_MODE=docker
    MUSLY="docker:${MUSLY_IMAGE}"
    return 0
  fi
  cat >&2 <<EOF
musly binary not found.

Build one of:
  docker build -t musly:dev .
  cmake -S . -B build && cmake --build build -j --target musly

Then re-run this script (Docker image musly:dev is used automatically),
or set MUSLY=/path/to/musly explicitly.
EOF
  exit 1
}

# DATA_ROOT: directory that contains both tree/ and fma_small/ (for symlinks).
_musly_data_root() {
  if [[ -n "${DATA_ROOT:-}" ]]; then
    echo "$DATA_ROOT"
    return
  fi
  # Default: parent of TREE (eval/data when TREE=eval/data/tree).
  echo "$(cd "$TREE/.." && pwd)"
}

# Map a host path to the path inside the eval container mounts.
_musly_container_path() {
  local host="$1"
  local abspath
  abspath="$(cd "$(dirname "$host")" && pwd)/$(basename "$host")"
  local data_root
  data_root="$(_musly_data_root)"
  case "$abspath" in
    "$TREE"/*) echo "/data/tree/${abspath#"$TREE"/}" ;;
    "$TREE") echo "/data/tree" ;;
    "$RUN_DIR"/*) echo "/data/run/${abspath#"$RUN_DIR"/}" ;;
    "$RUN_DIR") echo "/data/run" ;;
    "$data_root"/*) echo "/data/${abspath#"$data_root"/}" ;;
    "$data_root") echo "/data" ;;
    "${PERT:-__none__}"/*) echo "/data/pert/${abspath#"$PERT"/}" ;;
    "${PERT:-__none__}") echo "/data/pert" ;;
    "${OUT:-__none__}"/*) echo "/data/out/${abspath#"$OUT"/}" ;;
    "${OUT:-__none__}") echo "/data/out" ;;
    *)
      echo "Path not mounted into musly container: $host" >&2
      echo "Expected under TREE=$TREE or RUN_DIR=$RUN_DIR" >&2
      return 1
      ;;
  esac
}

# Rewrite absolute host paths in musly argv to container paths.
_musly_rewrite_args() {
  local -a out=()
  local arg next
  while [[ $# -gt 0 ]]; do
    arg="$1"; shift
    case "$arg" in
      -c|-s|-m|-j|-a|-p|-r)
        out+=("$arg")
        if [[ $# -gt 0 ]]; then
          next="$1"; shift
          if [[ "$next" == /* ]]; then
            out+=("$(_musly_container_path "$next")")
          else
            out+=("$next")
          fi
        fi
        ;;
      /*)
        if [[ -e "$arg" || "$arg" == "$TREE"* || "$arg" == "$RUN_DIR"* \
              || ( -n "${PERT:-}" && "$arg" == "$PERT"* ) \
              || ( -n "${OUT:-}" && "$arg" == "$OUT"* ) ]]; then
          out+=("$(_musly_container_path "$arg")")
        else
          out+=("$arg")
        fi
        ;;
      *) out+=("$arg") ;;
    esac
  done
  MUSLY_REWRITTEN_ARGS=("${out[@]}")
}

# Run musly with cwd on the host remapped into the container when needed.
# Usage: musly_run <host_workdir> -- [musly options...]
musly_run() {
  local host_cwd="$1"
  shift
  if [[ "${1:-}" == "--" ]]; then
    shift
  fi
  if [[ "$MUSLY_MODE" == "local" ]]; then
    (cd "$host_cwd" && "$MUSLY" "$@")
    return
  fi

  local c_cwd data_root
  c_cwd="$(_musly_container_path "$host_cwd")"
  data_root="$(_musly_data_root)"

  local -a docker_args=(
    run --rm
    -e "OMP_NUM_THREADS=${OMP_NUM_THREADS:-1}"
    # Mount the whole data dir so tree/ → ../fma_small/... relative symlinks work.
    -v "$data_root:/data"
    -v "$RUN_DIR:/data/run"
  )
  # TREE is normally $data_root/tree; if the user pointed TREE elsewhere, mount it too.
  if [[ "$(cd "$TREE" && pwd)" != "$(cd "$data_root/tree" 2>/dev/null && pwd)" ]]; then
    docker_args+=(-v "$TREE:/data/tree")
  fi
  if [[ -n "${PERT:-}" && -d "${PERT:-}" ]]; then
    docker_args+=(-v "$PERT:/data/pert")
  fi
  if [[ -n "${OUT:-}" && -d "${OUT:-}" ]]; then
    docker_args+=(-v "$OUT:/data/out")
  fi
  # Match host uid so result files are writable/owned by the user.
  if [[ "$(id -u)" != "0" ]]; then
    docker_args+=(--user "$(id -u):$(id -g)")
  fi
  docker_args+=(-w "$c_cwd" "$MUSLY_IMAGE")

  _musly_rewrite_args "$@"
  # stdbuf keeps progress lines visible without a TTY (Docker otherwise
  # block-buffers musly stdout, which looks like a hang after "Read 0").
  docker "${docker_args[@]}" stdbuf -oL -eL musly "${MUSLY_REWRITTEN_ARGS[@]}"
}
