# Sourced by the PS5 scripts: ps5/local.env (this machine's paths, see local.env.example), without overriding variables
# already set in the environment.
_pkp_env=$(dirname -- "${BASH_SOURCE[0]}")/local.env
if [[ -f $_pkp_env ]]; then
    while IFS= read -r _l || [[ -n $_l ]]; do
        [[ $_l =~ ^[[:space:]]*([A-Za-z_][A-Za-z0-9_]*)=(.*)$ ]] || continue
        _k=${BASH_REMATCH[1]}
        [[ -n ${!_k:-} ]] && continue
        eval "export $_k=\"${BASH_REMATCH[2]}\""
    done < "$_pkp_env"
fi
unset _pkp_env _l _k
# Required: fails with the variable's name and where to set it.
pkp_need() { local k; for k; do [[ -n ${!k:-} ]] || { printf 'error: %s is not set (ps5/local.env, see ps5/local.env.example)\n' "$k" >&2; exit 2; }; done; }
