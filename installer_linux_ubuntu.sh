#!/usr/bin/env bash

set -Eeuo pipefail

repo_url="https://github.com/Valephnull/RFF-EXP.git"
install_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
version_file="$install_root/version.config"
source_dir="$install_root/.rff-exp-source"
staging_dir="$install_root/.rff-exp-installing"
backup_dir="$install_root/.rff-exp-backup"
tools_dir="$install_root/.rff-exp-tools"
target_dirs=(res bin shaders)

step() {
    printf '\n\033[36m==== %s ====\033[0m\n' "$1"
}

restore_previous_install() {
    local name

    [[ -d "$backup_dir" ]] || return 0

    for name in "${target_dirs[@]}"; do
        if [[ -e "$backup_dir/$name" ]]; then
            rm -rf -- "$install_root/$name"
            mv -- "$backup_dir/$name" "$install_root/$name"
        elif [[ -e "$backup_dir/$name.absent" ]]; then
            rm -rf -- "$install_root/$name"
        fi
    done

    if [[ -f "$backup_dir/version.config" ]]; then
        mv -f -- "$backup_dir/version.config" "$version_file"
    elif [[ -e "$backup_dir/version.config.absent" ]]; then
        rm -f -- "$version_file"
    fi

    rm -f -- "$version_file.tmp"
    rm -rf -- "$backup_dir" "$staging_dir"
}

if [[ -d "$backup_dir" ]]; then
    step "Recovering an interrupted installation"
    restore_previous_install
fi

step "Installing Ubuntu build dependencies"

apt_command=(apt)
if (( EUID != 0 )); then
    if ! command -v sudo > /dev/null; then
        printf '\033[31msudo is required when the installer is not run as root.\033[0m\n' >&2
        exit 1
    fi
    apt_command=(sudo apt)
fi

"${apt_command[@]}" update
"${apt_command[@]}" install -y \
    build-essential \
    clang \
    cmake \
    git \
    libglfw3-dev \
    libglm-dev \
    libgmp-dev \
    libgtk-3-dev \
    libopencv-dev \
    libvulkan-dev \
    ninja-build \
    pkg-config \
    python3-venv

step "Checking the installed version"

newest_sha="$(git ls-remote "$repo_url" HEAD | awk 'NR == 1 { print $1 }')"
if [[ -z "$newest_sha" ]]; then
    printf '\033[31mCould not read the latest RFF-EXP revision.\033[0m\n' >&2
    exit 1
fi

installed_sha=""
if [[ -f "$version_file" ]]; then
    installed_sha="$(tr -d '[:space:]' < "$version_file")"
fi

install_complete=true
for name in "${target_dirs[@]}"; do
    [[ -e "$install_root/$name" ]] || install_complete=false
done

if [[ "$installed_sha" == "$newest_sha" && "$install_complete" == true && -x "$install_root/bin/RFF" ]]; then
    printf '\033[32mRFF-EXP is already up to date (%s).\033[0m\n' "${newest_sha:0:7}"
    if [[ -t 0 ]]; then
        read -r -p "Press Enter to exit" _ || true
    fi
    exit 0
fi

step "Checking the build tools"

if ! printf '#include <format>\n#include <locale>\nint main() { auto text = std::format(std::locale::classic(), "{}", 1); }\n' |
    clang++ -std=c++20 -x c++ -fsyntax-only -; then
    printf '\033[31mThe installed C++ standard library does not provide the C++20 features required by RFF-EXP.\033[0m\n' >&2
    printf '\033[31mUbuntu 24.04 or newer is required by the current RFF source.\033[0m\n' >&2
    exit 1
fi

cmake_command="$(command -v cmake || true)"
cmake_version=""
if [[ -n "$cmake_command" ]]; then
    cmake_version="$($cmake_command --version | awk 'NR == 1 { print $3 }')"
fi

if [[ -z "$cmake_version" ]] || ! dpkg --compare-versions "$cmake_version" ge "3.30"; then
    printf '\033[33mCMake 3.30 or newer is required; installing a private copy for RFF-EXP.\033[0m\n'
    rm -rf -- "$tools_dir"
    python3 -m venv "$tools_dir"
    "$tools_dir/bin/python" -m pip install --disable-pip-version-check --upgrade "cmake>=3.30,<5"
    cmake_command="$tools_dir/bin/cmake"
fi

printf 'Using %s\n' "$($cmake_command --version | awk 'NR == 1')"

step "Downloading RFF-EXP"

rm -rf -- "$source_dir" "$staging_dir" "$backup_dir"
git clone --depth 1 "$repo_url" "$source_dir"

step "Downloading external source dependencies"

mkdir -p "$source_dir/extern"
while IFS= read -r dependency_url; do
    dependency_url="${dependency_url%%#*}"
    dependency_url="$(printf '%s' "$dependency_url" | xargs)"
    [[ -z "$dependency_url" ]] && continue
    dependency_name="$(basename "${dependency_url%.git}")"
    git clone --depth 1 "$dependency_url" "$source_dir/extern/$dependency_name"
done < "$source_dir/extern_sources"

step "Building RFF-EXP"

"$cmake_command" -S "$source_dir" -B "$source_dir/build" -G Ninja \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_BUILD_TYPE=Release
"$cmake_command" --build "$source_dir/build" --parallel "$(nproc)"

step "Installing RFF-EXP"

mkdir -p "$staging_dir" "$backup_dir"
for name in "${target_dirs[@]}"; do
    if [[ ! -e "$source_dir/$name" ]]; then
        printf '\033[31mThe completed build is missing %s.\033[0m\n' "$name" >&2
        exit 1
    fi
    cp -a -- "$source_dir/$name" "$staging_dir/$name"
done

backed_up=()
installed=()
rollback_install() {
    set +e
    for name in "${installed[@]}"; do
        rm -rf -- "$install_root/$name"
    done
    for name in "${backed_up[@]}"; do
        if [[ -e "$backup_dir/$name" ]]; then
            mv -- "$backup_dir/$name" "$install_root/$name"
        fi
    done

    rm -f -- "$version_file" "$version_file.tmp"
    if [[ -f "$backup_dir/version.config" ]]; then
        mv -- "$backup_dir/version.config" "$version_file"
    fi

    rm -rf -- "$backup_dir" "$staging_dir"
}

deployment_failed() {
    local status=$?
    trap - ERR INT TERM
    rollback_install
    exit "$status"
}

deployment_interrupted() {
    local status="$1"
    trap - ERR INT TERM
    rollback_install
    exit "$status"
}

trap deployment_failed ERR
trap 'deployment_interrupted 130' INT
trap 'deployment_interrupted 143' TERM

if [[ -f "$version_file" ]]; then
    mv -- "$version_file" "$backup_dir/version.config"
else
    : > "$backup_dir/version.config.absent"
fi

for name in "${target_dirs[@]}"; do
    if [[ -e "$install_root/$name" ]]; then
        mv -- "$install_root/$name" "$backup_dir/$name"
        backed_up+=("$name")
    else
        : > "$backup_dir/$name.absent"
    fi
done

for name in "${target_dirs[@]}"; do
    mv -- "$staging_dir/$name" "$install_root/$name"
    installed+=("$name")
done

printf '%s' "$newest_sha" > "$version_file.tmp"
mv -- "$version_file.tmp" "$version_file"
trap - ERR INT TERM

rm -rf -- "$backup_dir" "$staging_dir" "$source_dir"

step "Installation finished"
printf '\033[32mLocation: %s\033[0m\n' "$install_root"
printf '\033[32mVersion:  %s\033[0m\n' "${newest_sha:0:7}"

launch=""
if [[ -t 0 ]]; then
    read -r -p "Launch RFF-EXP now? [y/N] " launch || true
fi
if [[ "$launch" =~ ^[Yy]$ ]]; then
    (cd "$install_root/bin" && ./RFF)
fi
