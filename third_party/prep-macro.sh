#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source_dir="${1:-${script_dir}/sources}"
commit=f49e9178767d557a522618b16ce8694f9ddac628

if (($# > 1)); then
    printf 'Usage: %s [source-dir]\n' "$0" >&2
    exit 2
fi

source "${script_dir}/../scripts/lib/common.sh"
spectra_load_config

oneapi_setvars="${ONEAPI_SETVARS:-/opt/intel/oneapi/setvars.sh}"
llama_bench="${LLAMA_BENCH:-${source_dir}/llama.cpp/build-spr-mkl/bin/llama-bench}"
model="${LLAMA_MODEL:-${source_dir}/llama.cpp/models/qwen/Qwen3-8B-GGUF/Qwen3-8B-Q4_K_M.gguf}"
faiss_flat="${FAISS_FLAT:-${source_dir}/faiss/build/tutorial/cpp/1-Flat}"
faiss_bench_search="${FAISS_BENCH_SEARCH:-${source_dir}/faiss/build/tutorial/cpp/bench_search}"
dramhit_bin="${DRAMHIT_BIN:-${source_dir}/DRAMHiT/build/dramhit}"
vht_bin="${VHT_BIN:-${source_dir}/vht/build_qp/qp-llc-bench}"
resource_dir="${SPECTRA_ROOT}/third_party/resources"
faiss_index="${FAISS_INDEX:-${resource_dir}/SIFT100M_IVF128,PQ32_populated.index}"
faiss_queries="${FAISS_QUERIES:-${resource_dir}/bigann_50w_queries.bvecs}"
resource_release="https://github.com/zelenyaz/spectra/releases/download/resources-v1"

require_command() {
    command -v "$1" >/dev/null || { printf 'Missing command: %s\n' "$1" >&2; exit 1; }
}

check_sha256() {
    printf '%s  %s\n' "$1" "$2" | sha256sum --check --status
}

download_resource_asset() {
    local name="$1" expected="$2" path="${resource_cache}/${1}"
    if [[ -f "$path" ]] && check_sha256 "$expected" "$path"; then
        return
    fi
    rm -f -- "$path"
    if [[ -f "${path}.part" ]] && check_sha256 "$expected" "${path}.part"; then
        mv -- "${path}.part" "$path"
        return
    fi
    printf '[macro] Downloading %s\n' "$name"
    curl -fL -C - --retry 3 -o "${path}.part" "${resource_release}/${name}"
    if ! check_sha256 "$expected" "${path}.part"; then
        rm -f -- "${path}.part"
        printf 'SHA-256 mismatch: %s\n' "$name" >&2
        return 1
    fi
    mv -- "${path}.part" "$path"
}

restore_faiss_resource() {
    local kind="$1" target="$2" expected="$3" output
    mkdir -p -- "$(dirname -- "$target")"
    output="$(mktemp "${target}.tmp.XXXXXXXX")"
    if [[ "$kind" == index ]]; then
        if ! cat "${resource_cache}/SIFT100M_IVF128.PQ32_populated.index.zst.part-"{001,002,003} | zstd -dc > "$output"; then
            rm -f -- "$output"
            return 1
        fi
    else
        if ! zstd -dc "${resource_cache}/bigann_50w_queries.bvecs.zst" > "$output"; then
            rm -f -- "$output"
            return 1
        fi
    fi
    if ! check_sha256 "$expected" "$output"; then
        rm -f -- "$output"
        printf 'SHA-256 mismatch after restoring %s\n' "$target" >&2
        return 1
    fi
    mv -- "$output" "$target"
    printf '[macro] Restored %s\n' "$target"
}

prepare_faiss_resources() {
    if [[ -s "$faiss_index" && -s "$faiss_queries" ]]; then
        return
    fi
    require_command curl
    require_command zstd
    require_command sha256sum
    resource_cache="${resource_dir}/.release-cache"
    mkdir -p -- "$resource_cache"

    if [[ ! -s "$faiss_index" ]]; then
        download_resource_asset SIFT100M_IVF128.PQ32_populated.index.zst.part-001 \
            e8fa94c2439f1fc613b2d900ef9e8fe8c55c200dabe248c93426be89f9d28937
        download_resource_asset SIFT100M_IVF128.PQ32_populated.index.zst.part-002 \
            a743e77a08af9225be7562bbdf37bd07461ed0b231672c0fe457de22aa433cc9
        download_resource_asset SIFT100M_IVF128.PQ32_populated.index.zst.part-003 \
            b2c6728ee49625e6a920c49f24639d7973e5320c8d42efcd3c8b9f11c159a0e4
        restore_faiss_resource index "$faiss_index" \
            677c5562274ed2ece103bf9fee43894c224cd0d7ba7f944542e12fd2fd00aca3
        rm -f -- "${resource_cache}/SIFT100M_IVF128.PQ32_populated.index.zst.part-"{001,002,003}
    fi
    if [[ ! -s "$faiss_queries" ]]; then
        download_resource_asset bigann_50w_queries.bvecs.zst \
            a85ebc3517b9015684ac944faa271ca7c7659f259b8cbea91b2eb68b73921c29
        restore_faiss_resource queries "$faiss_queries" \
            1833404e354901bcf611d7085afaa4c817a369ee2ce59757d6eb7afd1ca62148
        rm -f -- "${resource_cache}/bigann_50w_queries.bvecs.zst"
    fi
    rmdir -- "$resource_cache" 2>/dev/null || true
}

install_oneapi() {
    if [[ -f "$oneapi_setvars" ]]; then
        return
    fi
    for command in sudo apt-get gpg; do
        command -v "$command" >/dev/null || { printf 'Installing oneAPI requires %s\n' "$command" >&2; exit 1; }
    done
    local keyring=/usr/share/keyrings/oneapi-archive-keyring.gpg
    local source_list=/etc/apt/sources.list.d/oneapi.list
    local temp_key
    temp_key="$(mktemp)"
    if [[ ! -f "$keyring" ]]; then
        require_command curl
        curl -fL --retry 3 https://apt.repos.intel.com/intel-gpg-keys/GPG-PUB-KEY-INTEL-SW-PRODUCTS.PUB -o "$temp_key"
        gpg --dearmor < "$temp_key" | sudo tee "$keyring" >/dev/null
    fi
    rm -f -- "$temp_key"
    if [[ ! -f "$source_list" ]]; then
        printf 'deb [signed-by=%s] https://apt.repos.intel.com/oneapi all main\n' "$keyring" | sudo tee "$source_list" >/dev/null
    fi
    sudo apt-get update
    sudo apt-get install -y intel-oneapi-mkl-devel intel-oneapi-compiler-dpcpp-cpp
    [[ -f "$oneapi_setvars" ]] || { printf 'oneAPI setup script missing: %s\n' "$oneapi_setvars" >&2; exit 1; }
}

prepare_checkout() {
    local dir="$1" url="$2" revision="$3" kind="${4:-commit}"
    local resolved
    require_command git
    if [[ ! -e "$dir" ]]; then
        git clone "$url" "$dir"
    fi
    [[ -d "$dir/.git" ]] || { printf 'Not a Git checkout: %s\n' "$dir" >&2; exit 1; }
    if [[ "$kind" == tag ]]; then
        if ! git -C "$dir" rev-parse -q --verify "refs/tags/${revision}^{commit}" >/dev/null; then
            git -C "$dir" fetch origin tag "$revision"
        fi
        resolved="$(git -C "$dir" rev-parse "refs/tags/${revision}^{commit}")"
    else
        if ! git -C "$dir" cat-file -e "${revision}^{commit}" 2>/dev/null; then
            git -C "$dir" fetch origin "$revision"
        fi
        resolved="$revision"
    fi
    if [[ "$(git -C "$dir" rev-parse HEAD)" != "$resolved" ]]; then
        if [[ -n "$(git -C "$dir" status --porcelain)" ]]; then
            printf 'Checkout has local changes; refusing to switch commits: %s\n' "$dir" >&2
            exit 1
        fi
        git -C "$dir" checkout --detach "$revision"
    fi
}

apply_project_patch() {
    local dir="$1" name="$2" patch="$3"
    if git -C "$dir" apply --reverse --check "$patch" 2>/dev/null; then
        printf '[macro] %s patch already applied\n' "$name"
    elif git -C "$dir" apply --check "$patch"; then
        git -C "$dir" apply "$patch"
    else
        printf 'Cannot apply patch: %s\n' "$patch" >&2
        exit 1
    fi
}

if [[ ! -x "$llama_bench" || ! -x "$faiss_flat" || ! -x "$faiss_bench_search" ]]; then
    require_command cmake
    install_oneapi
    # setvars.sh references optional variables under nounset.
    set +u
    source "$oneapi_setvars" >/dev/null
    set -u
    command -v icx >/dev/null && command -v icpx >/dev/null || { printf 'Intel C/C++ compilers unavailable after sourcing %s\n' "$oneapi_setvars" >&2; exit 1; }
    [[ -n "${MKLROOT:-}" ]] || { printf 'MKLROOT unavailable after sourcing %s\n' "$oneapi_setvars" >&2; exit 1; }
fi

if [[ ! -x "$llama_bench" ]]; then
    mkdir -p -- "$source_dir"
    llama_dir="${source_dir}/llama.cpp"
    prepare_checkout "$llama_dir" https://github.com/ggml-org/llama.cpp "$commit"
    cmake -S "$llama_dir" -B "$llama_dir/build-spr-mkl" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=icx -DCMAKE_CXX_COMPILER=icpx \
        -DGGML_BLAS=ON -DGGML_BLAS_VENDOR=Intel10_64lp -DGGML_NATIVE=ON \
        -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_SERVER=OFF
    cmake --build "$llama_dir/build-spr-mkl" --target llama-bench -j "${JOBS:-$(nproc)}"
    llama_bench="${llama_dir}/build-spr-mkl/bin/llama-bench"
fi

if [[ ! -s "$model" ]]; then
    require_command curl
    mkdir -p -- "$(dirname -- "$model")"
    curl -fL -C - --retry 3 -o "${model}.part" \
        https://huggingface.co/Qwen/Qwen3-8B-GGUF/resolve/main/Qwen3-8B-Q4_K_M.gguf
    mv -- "${model}.part" "$model"
fi

printf '[macro] LLAMA_BENCH=%s\n[macro] LLAMA_MODEL=%s\n' "$llama_bench" "$model"

if [[ ! -x "$faiss_flat" || ! -x "$faiss_bench_search" ]]; then
    faiss_targets=()
    [[ -x "$faiss_flat" ]] || faiss_targets+=(1-Flat)
    [[ -x "$faiss_bench_search" ]] || faiss_targets+=(bench_search)
    mkdir -p -- "$source_dir"
    faiss_dir="${source_dir}/faiss"
    prepare_checkout "$faiss_dir" https://github.com/facebookresearch/faiss v1.14.1 tag

    faiss_patch="${script_dir}/patches/faiss.patch"
    apply_project_patch "$faiss_dir" FAISS "$faiss_patch"

    compiler_tools="$(dirname -- "$(command -v icx)")/compiler"
    cmake -S "$faiss_dir" -B "$faiss_dir/build" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=icx -DCMAKE_CXX_COMPILER=icpx \
        -DCMAKE_AR="${compiler_tools}/llvm-ar" -DCMAKE_RANLIB="${compiler_tools}/llvm-ranlib" \
        -DCMAKE_C_COMPILER_AR="${compiler_tools}/llvm-ar" -DCMAKE_CXX_COMPILER_AR="${compiler_tools}/llvm-ar" \
        -DCMAKE_C_COMPILER_RANLIB="${compiler_tools}/llvm-ranlib" -DCMAKE_CXX_COMPILER_RANLIB="${compiler_tools}/llvm-ranlib" \
        -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=${compiler_tools}/ld.lld" \
        -DCMAKE_SHARED_LINKER_FLAGS="-fuse-ld=${compiler_tools}/ld.lld" \
        -DFAISS_OPT_LEVEL=avx512_spr -DFAISS_ENABLE_MKL=ON -DFAISS_USE_LTO=ON \
        -DFAISS_ENABLE_GPU=OFF -DFAISS_ENABLE_PYTHON=OFF -DFAISS_ENABLE_SVS=OFF \
        -DBUILD_TESTING=OFF
    cmake --build "$faiss_dir/build" --target "${faiss_targets[@]}" -j "${JOBS:-$(nproc)}"
    [[ -x "$faiss_flat" ]] || faiss_flat="${faiss_dir}/build/tutorial/cpp/1-Flat"
    [[ -x "$faiss_bench_search" ]] || faiss_bench_search="${faiss_dir}/build/tutorial/cpp/bench_search"
fi
printf '[macro] FAISS_FLAT=%s\n[macro] FAISS_BENCH_SEARCH=%s\n' \
    "$faiss_flat" "$faiss_bench_search"
prepare_faiss_resources
printf '[macro] FAISS_INDEX=%s\n[macro] FAISS_QUERIES=%s\n' \
    "$faiss_index" "$faiss_queries"

if [[ ! -x "$dramhit_bin" ]]; then
    require_command cmake
    mkdir -p -- "$source_dir"
    dramhit_dir="${source_dir}/DRAMHiT"
    dramhit_commit=112046bbbd879fc2ae7f2c3141834d438603c9b7
    prepare_checkout "$dramhit_dir" https://github.com/mars-research/DRAMHiT.git "$dramhit_commit"
    git -C "$dramhit_dir" submodule update --init --recursive

    dramhit_patch="${script_dir}/patches/dramhit.patch"
    apply_project_patch "$dramhit_dir" DRAMHiT "$dramhit_patch"

    cmake -S "$dramhit_dir" -B "$dramhit_dir/build" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=/usr/bin/cc \
        -DCMAKE_CXX_COMPILER=/usr/bin/c++ -DCPUFREQ_MHZ=2000 \
        -DDRAMHiT_VARIANT=2023 -DBENCHMARK_BACKEND=NONE \
        -DBUILD_APP=ON -DBUILD_EXAMPLE=OFF -DBUILD_TESTING=OFF
    cmake --build "$dramhit_dir/build" --target dramhit -j "${JOBS:-$(nproc)}"
    dramhit_bin="${dramhit_dir}/build/dramhit"
fi
printf '[macro] DRAMHIT_BIN=%s\n' "$dramhit_bin"

if [[ ! -x "$vht_bin" ]]; then
    require_command cmake
    mkdir -p -- "$source_dir"
    vht_dir="${source_dir}/vht"
    vht_commit=ee521bf9c8b8b062b994637d53ae83b98d9fa5c5
    prepare_checkout "$vht_dir" https://github.com/hpides/vectorized-hash-tables.git "$vht_commit"

    vht_patch="${script_dir}/patches/vht.patch"
    apply_project_patch "$vht_dir" VHT "$vht_patch"

    cmake -S "$vht_dir" -B "$vht_dir/build_qp" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=/usr/bin/cc \
        -DCMAKE_CXX_COMPILER=/usr/bin/c++ -DHASHMAP_BUILD_EXTERNAL=OFF
    cmake --build "$vht_dir/build_qp" --target qp-llc-bench -j "${JOBS:-$(nproc)}"
    vht_bin="${vht_dir}/build_qp/qp-llc-bench"
fi
printf '[macro] VHT_BIN=%s\n' "$vht_bin"
