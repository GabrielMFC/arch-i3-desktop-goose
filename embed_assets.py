import os

ASSETS_DIR = "./Assets"
OUTPUT_DIR = "./embed"

def ensure_dir(path):
    if not os.path.exists(path):
        os.makedirs(path)

def sanitize(name: str) -> str:
    # transforma path em nome seguro C
    return (
        name.replace("\\", "/")
            .replace("/", "_")
            .replace(".", "_")
            .replace("-", "_")
    )

def to_c_header(var_name: str, data: bytes) -> str:
    hex_bytes = ", ".join(f"0x{b:02x}" for b in data)

    return f"""#pragma once

unsigned char {var_name}[] = {{
    {hex_bytes}
}};

unsigned int {var_name}_size = {len(data)};
"""

def main():
    ensure_dir(OUTPUT_DIR)

    for root, _, files in os.walk(ASSETS_DIR):
        for file in files:
            full_path = os.path.join(root, file)

            # path relativo dentro de Assets
            rel_path = os.path.relpath(full_path, ASSETS_DIR)

            # nome único baseado no path inteiro (flat)
            var_name = sanitize(rel_path)

            # saída SEM PASTA (tudo direto em embed/)
            out_file = os.path.join(OUTPUT_DIR, var_name + ".h")

            with open(full_path, "rb") as f:
                data = f.read()

            header = to_c_header(var_name, data)

            with open(out_file, "w", encoding="utf-8") as f:
                f.write(header)

            print(f"[OK] {rel_path} -> {out_file}")

if __name__ == "__main__":
    main()