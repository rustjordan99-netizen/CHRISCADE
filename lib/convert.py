import re

def rgb565_to_rgb_components(hex_value):
    value = int(hex_value, 16)
    r = (value >> 11) & 0x1F  # 5 bits
    g = (value >> 5) & 0x3F   # 6 bits
    b = value & 0x1F          # 5 bits
    return f"COLOR(0x{r:02X}, 0x{g:02X}, 0x{b:02X})"

def convert_colors(text):
    pattern = r'COLOR\(0x([0-9A-Fa-f]{4})\)'
    return re.sub(pattern, lambda m: rgb565_to_rgb_components(m.group(1)), text)

def process_file(input_path, output_path):
    with open(input_path, 'r', encoding='utf-8') as infile:
        content = infile.read()
    converted = convert_colors(content)
    with open(output_path, 'w', encoding='utf-8') as outfile:
        outfile.write(converted)

# Beispielnutzung
input_file = 'gbcolors.h'
output_file = 'gbcolors.h2'
process_file(input_file, output_file)
print(f"Konvertierte Datei gespeichert unter: {output_file}")
