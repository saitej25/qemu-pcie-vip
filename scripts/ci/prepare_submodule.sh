#!/usr/bin/env bash
set -euo pipefail

# Fetch only the submodule needed by the current CI job.  GitLab occasionally
# returns 503 for the large QEMU repository, so retry and transparently use
# the official GitHub mirror for that submodule.

if [[ $# -lt 1 ]]; then
    echo "usage: $0 SUBMODULE_PATH [...]" >&2
    exit 2
fi

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${ROOT_DIR}"

for path in "$@"; do
    url="$(git config -f .gitmodules --get "submodule.${path}.url")"
    fallback=""
    case "${path}" in
        qemu) fallback="https://github.com/qemu/qemu.git" ;;
        third_party/buildroot) fallback="https://github.com/buildroot/buildroot.git" ;;
    esac

    # Keep the gitlink's exact reviewed commit; only the transport URL is
    # changed.  This also works with shallow GitHub Actions checkouts.
    if [[ -n "${fallback}" ]]; then
        git config --global "url.${fallback}.insteadOf" "${url}"
    fi

    success=0
    for attempt in 1 2 3 4; do
        if git submodule update --init --depth=1 "${path}"; then
            success=1
            break
        fi
        echo "submodule ${path}: attempt ${attempt} failed; retrying" >&2
        sleep $((attempt * 5))
    done
    if [[ "${success}" != 1 ]]; then
        echo "unable to fetch required submodule: ${path}" >&2
        exit 1
    fi
done
