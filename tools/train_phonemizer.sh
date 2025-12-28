#!/bin/bash
# Train phonemizer using espeak-ng (development tool only, not distributed)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DATA_DIR="$PROJECT_ROOT/src/tts/phonemizer/data"
VENV_DIR="$SCRIPT_DIR/venv"

echo "🎓 EthervoxAI Phonemizer Training Tool"
echo "========================================"
echo ""
echo "This tool uses espeak-ng locally to generate training data."
echo "espeak is NOT included in the distributed software."
echo ""

# Check for espeak-ng
if ! command -v espeak-ng &> /dev/null; then
    echo "❌ espeak-ng not found!"
    echo ""
    echo "Install instructions:"
    echo "  macOS:   brew install espeak-ng"
    echo "  Ubuntu:  sudo apt install espeak-ng"
    echo "  Windows: choco install espeak-ng"
    echo ""
    exit 1
fi

# Setup Python virtual environment
if [ ! -d "$VENV_DIR" ]; then
    echo "📦 Creating Python virtual environment..."
    python3 -m venv "$VENV_DIR"
    echo "✅ Virtual environment created at: $VENV_DIR"
fi

# Activate virtual environment
echo "🔧 Activating virtual environment..."
source "$VENV_DIR/bin/activate"

# Install/upgrade required packages
echo "📥 Installing Python dependencies..."
pip install --upgrade pip --quiet
pip install phonemizer --quiet

echo "✅ Python environment ready"
echo ""

mkdir -p "$DATA_DIR"

# Generate dictionaries for each language
echo ""
echo "📚 Generating pronunciation dictionaries..."
echo ""

# English (US)
echo "▶ English (US)..."
python3 "$SCRIPT_DIR/generate_espeak_dict.py" \
    --lang en-us \
    --output "$DATA_DIR/espeak_en_us.dict" \
    --format dict

# English (GB)
echo ""
echo "▶ English (GB)..."
python3 "$SCRIPT_DIR/generate_espeak_dict.py" \
    --lang en-gb \
    --output "$DATA_DIR/espeak_en_gb.dict" \
    --format dict

# German
echo ""
echo "▶ German..."
python3 "$SCRIPT_DIR/generate_espeak_dict.py" \
    --lang de \
    --output "$DATA_DIR/espeak_de.dict" \
    --format dict

# Now generate C headers for embedding
echo ""
echo "📝 Generating C headers for embedded dictionaries..."
echo ""

for lang in en-us en-gb de; do
    echo "▶ Generating header for $lang..."
    dict_file="$DATA_DIR/espeak_${lang//-/_}.dict"
    header_file="$DATA_DIR/espeak_dict_${lang//-/_}.h"
    
    if [ -f "$dict_file" ]; then
        python3 "$SCRIPT_DIR/generate_espeak_dict.py" \
            --lang "$lang" \
            --output "$header_file" \
            --format header \
            --dict-file "$dict_file"
    fi
done

echo ""
echo "✅ Training complete!"
echo ""
echo "Generated files:"
ls -lh "$DATA_DIR"/espeak_* 2>/dev/null | awk '{print "  " $9 " (" $5 ")"}' || echo "  (No files generated)"
echo ""
echo "Next steps:"
echo "  1. Review generated dictionaries in: $DATA_DIR"
echo "  2. Rebuild phonemizer with: npm run build:core"
echo "  3. Test pronunciation accuracy against espeak output"
echo ""
echo "The phonemizer will now use espeak-trained pronunciations!"

# Deactivate virtual environment
deactivate
