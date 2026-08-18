#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
server_build_dir="${repo_root}/build/server"
lab_build_dir="${repo_root}/build/server_lab"
launch_gui=true
if [[ "${1:-}" == "--no-launch" ]]; then
    launch_gui=false
fi

case "$(uname -s)" in
    Darwin|Linux) ;;
    *)
        echo "Unsupported shell host. On Windows, run: powershell -ExecutionPolicy Bypass -File .\\start.ps1" >&2
        exit 1
        ;;
esac

command -v cmake >/dev/null 2>&1 || {
    echo "CMake is required. Install CMake and run ./start.sh again." >&2
    exit 1
}

find_qt_prefix(){
    for qt_tool in qmake6 qmake; do
        if command -v "${qt_tool}" >/dev/null 2>&1; then
            "${qt_tool}" -query QT_INSTALL_PREFIX 2>/dev/null && return 0
        fi
    done
    return 1
}

install_qt(){
    echo "Qt Widgets was not found. Installing Qt..."
    case "$(uname -s)" in
        Darwin)
            command -v brew >/dev/null 2>&1 || {
                echo "Homebrew is required for automatic Qt installation: https://brew.sh" >&2
                exit 1
            }
            brew install qt
            ;;
        Linux)
            if command -v apt-get >/dev/null 2>&1; then
                sudo apt-get update
                sudo apt-get install -y qt6-base-dev
            elif command -v dnf >/dev/null 2>&1; then
                sudo dnf install -y qt6-qtbase-devel
            elif command -v pacman >/dev/null 2>&1; then
                sudo pacman -S --needed qt6-base
            else
                echo "Unsupported Linux package manager. Install the Qt 6 Widgets development package manually." >&2
                exit 1
            fi
            ;;
    esac
}

qt_prefix="${SERVER_LAB_QT_PREFIX:-}"
if [[ -z "${qt_prefix}" ]]; then
    qt_prefix="$(find_qt_prefix || true)"
fi
if [[ -z "${qt_prefix}" ]]; then
    install_qt
    qt_prefix="$(find_qt_prefix || true)"
    if [[ -z "${qt_prefix}" && "$(uname -s)" == Darwin ]]; then
        qt_prefix="$(brew --prefix qt)"
    fi
fi

prepare_docker_lab(){
    [[ "$(uname -s)" == Darwin ]] || return 0
    [[ "${launch_gui}" == true ]] || return 0
    if ! command -v docker >/dev/null 2>&1; then
        read -r -p "Docker Desktop이 없습니다. v4/v5 실행을 위해 설치할까요? [y/N] " answer
        if [[ "${answer}" =~ ^[Yy]$ ]]; then
            command -v brew >/dev/null 2>&1 || {
                echo "Docker Desktop 자동 설치에는 Homebrew가 필요합니다: https://brew.sh" >&2
                return 0
            }
            brew install --cask docker
        else
            return 0
        fi
    fi

    if ! docker info >/dev/null 2>&1; then
        open -a Docker
        echo "Docker Desktop 시작을 기다리는 중..."
        for _ in {1..60}; do
            docker info >/dev/null 2>&1 && break
            sleep 2
        done
    fi
    if ! docker info >/dev/null 2>&1; then
        echo "Docker daemon이 준비되지 않아 v4/v5는 비활성화됩니다." >&2
        return 0
    fi

    echo "Building Docker image for v4/v5..."
    docker build -f "${repo_root}/docker/Dockerfile.lab" -t cpp-server-lab:latest "${repo_root}"
}

prepare_docker_lab

echo "[1/5] Configuring platform server versions..."
cmake -S "${repo_root}/server" -B "${server_build_dir}"

echo "[2/5] Building platform server versions..."
cmake --build "${server_build_dir}" --parallel

echo "[3/5] Configuring Server Architecture Lab..."
lab_cmake_args=(-S "${repo_root}/server_lab" -B "${lab_build_dir}")
if [[ -n "${qt_prefix}" ]]; then
    lab_cmake_args+=("-DCMAKE_PREFIX_PATH=${qt_prefix}")
fi
if ! cmake "${lab_cmake_args[@]}"; then
    echo "Qt Widgets development files were not found." >&2
    echo "Install Qt 6 (or Qt 5.15), or set SERVER_LAB_QT_PREFIX to the Qt installation prefix." >&2
    exit 1
fi

echo "[4/5] Building and testing Server Architecture Lab..."
cmake --build "${lab_build_dir}" --parallel
ctest --test-dir "${lab_build_dir}" --output-on-failure

if [[ "$(uname -s)" == Darwin ]]; then
    app_path="${lab_build_dir}/MMO Server Lab.app"
    macdeployqt_path="${qt_prefix}/bin/macdeployqt"
    if [[ -x "${macdeployqt_path}" ]]; then
        echo "[macOS 1/4] Bundling Qt libraries and plugins..."
        deploy_started_at=$SECONDS
        "${macdeployqt_path}" "${app_path}" -always-overwrite &
        deploy_pid=$!
        while kill -0 "${deploy_pid}" 2>/dev/null; do
            sleep 5
            if kill -0 "${deploy_pid}" 2>/dev/null; then
                echo "  macdeployqt is still running... $((SECONDS - deploy_started_at))s"
            fi
        done
        wait "${deploy_pid}"
        echo "  Qt bundle completed in $((SECONDS - deploy_started_at))s."
    else
        echo "[macOS 1/4] macdeployqt not found; skipping Qt bundle deployment."
    fi
    # macdeployqt changes the bundle after the linker created its ad-hoc
    # signature. Re-sign the completed bundle so recent macOS versions do not
    # terminate it with CODESIGNING/Invalid Page before main() starts.
    echo "[macOS 2/4] Clearing extended attributes..."
    xattr -cr "${app_path}"
    echo "[macOS 3/4] Signing bundled libraries and plugins..."
    signed_count=0
    while IFS= read -r -d '' bundled_file; do
        if file "${bundled_file}" | grep -q 'Mach-O'; then
            if ! sign_output="$(codesign --force --sign - "${bundled_file}" 2>&1)"; then
                echo "${sign_output}" >&2
                exit 1
            fi
            signed_count=$((signed_count + 1))
        fi
    done < <(find "${app_path}/Contents/Frameworks" "${app_path}/Contents/PlugIns" -type f -print0)
    codesign --force --sign - "${app_path}/Contents/MacOS/MMO Server Lab" 2>/dev/null
    codesign --force --sign - "${app_path}" 2>/dev/null
    echo "  Signed ${signed_count} bundled components and the app executable."
    echo "[macOS 4/4] Verifying app signature..."
    codesign --verify --deep --strict "${app_path}"
    echo "  Signature verification passed."
    ln -sfn "build/server_lab/MMO Server Lab.app" "${repo_root}/MMO Server Lab.app"
    echo "  App ready: ${repo_root}/MMO Server Lab.app"
fi

echo "[5/5] Starting Server Architecture Lab..."
if [[ "${launch_gui}" == false ]]; then
    echo "Build and tests completed (--no-launch)."
    exit 0
fi
cd "${repo_root}"
if [[ "$(uname -s)" == Darwin ]]; then
    open "${lab_build_dir}/MMO Server Lab.app"
    sleep 1
    launched_pid="$(pgrep -x "MMO Server Lab" | head -1 || true)"
    if [[ -n "${launched_pid}" ]]; then
        echo "GUI launched successfully (PID ${launched_pid})."
    else
        echo "GUI launch was requested. macOS may still be starting the app."
    fi
else
    exec "${lab_build_dir}/MMO Server Lab"
fi
