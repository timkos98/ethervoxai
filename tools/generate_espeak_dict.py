#!/usr/bin/env python3
"""
Generate espeak-compatible pronunciation dictionary for offline training.

LICENSING NOTE - GPL-Safe Usage:
  This is a development tool only, NOT distributed software. It uses espeak-ng
  (GPL) locally to generate pronunciation data (facts, not copyrightable code).
  The generated .dict files are pure data and can be embedded under MIT license.
  Espeak-ng is NEVER linked or distributed with EthervoxAI.

This tool uses espeak-ng locally to generate IPA pronunciations for a large
vocabulary, which are then embedded in the phonemizer. This is legal since
espeak is only used as a development tool, not distributed in the software.

Usage:
    python3 generate_espeak_dict.py --lang en-us --output data/espeak_en_us.dict
    python3 generate_espeak_dict.py --lang de --output data/espeak_de.dict

Requirements:
    pip install phonemizer  # Wrapper for espeak-ng (GPL, dev tool only)
"""

import argparse
import sys
import os
import signal
from pathlib import Path
from multiprocessing import Pool, cpu_count
from functools import partial
try:
    from phonemizer import phonemize
    from phonemizer.backend import EspeakBackend
    HAVE_PHONEMIZER = True
except ImportError:
    HAVE_PHONEMIZER = False
    print("WARNING: phonemizer library not found. Install with: pip install phonemizer", file=sys.stderr)

# Global flag for graceful shutdown
_interrupted = False

def _signal_handler(signum, frame):
    """Handle Ctrl+C gracefully."""
    global _interrupted
    _interrupted = True
    print("\n\n⚠️  Interrupted by user. Cleaning up...", file=sys.stderr)
    sys.exit(130)  # Standard exit code for SIGINT

def load_vocabulary(lang):
    """Load comprehensive word list for the target language."""
    vocab = set()
    
    # 1. CMU Dictionary words (English)
    cmu_path = Path(__file__).parent.parent / "src/tts/phonemizer/data/cmudict-0.7b.txt"
    if lang.startswith('en') and cmu_path.exists():
        with open(cmu_path, 'r', encoding='latin-1') as f:
            for line in f:
                if line.startswith(';;;'):
                    continue
                word = line.split()[0].split('(')[0]  # Remove (n) variants
                vocab.add(word.lower())
        print(f"Loaded {len(vocab)} words from CMU dictionary")
    
    # 2. Common words list (language-specific)
    common_words = {
        'en': ['the', 'be', 'to', 'of', 'and', 'a', 'in', 'that', 'have', 'I',
               'it', 'for', 'not', 'on', 'with', 'he', 'as', 'you', 'do', 'at',
               'hello', 'world', 'voice', 'assistant', 'computer', 'phone',
               'please', 'thank', 'sorry', 'yes', 'no', 'okay', 'help'],
        'de': ['der', 'die', 'das', 'und', 'ist', 'nicht', 'ein', 'eine',
               'hallo', 'welt', 'bitte', 'danke', 'ja', 'nein', 'gut'],
        'zh': ['你好', '世界', '语音', '助手', '电脑', '手机', '谢谢', '对不起'],
        'es': ['el', 'la', 'de', 'que', 'y', 'a', 'en', 'un', 'ser', 'se',
               'hola', 'mundo', 'por favor', 'gracias', 'sí', 'no'],
    }
    
    lang_prefix = lang.split('-')[0]
    if lang_prefix in common_words:
        vocab.update(common_words[lang_prefix])
        print(f"Added {len(common_words[lang_prefix])} common {lang_prefix} words")
    
    # 3. Add contractions and variants for English
    if lang.startswith('en'):
        contractions = ["don't", "can't", "won't", "I'm", "you're", "it's", 
                       "we're", "they're", "isn't", "aren't", "wasn't", "weren't"]
        vocab.update(contractions)
    
    return sorted(vocab)

def generate_pronunciations(words, lang, batch_size=1000, num_workers=None):
    """Use espeak to generate IPA pronunciations (using phonemizer's built-in parallelization)."""
    if not HAVE_PHONEMIZER:
        print("ERROR: phonemizer library required. Install: pip install phonemizer", file=sys.stderr)
        sys.exit(1)
    
    # Map our language codes to espeak language codes
    lang_map = {
        'en-us': 'en-us',
        'en-gb': 'en-gb',
        'de': 'de',
        'zh': 'cmn',  # Mandarin Chinese
        'es-mx': 'es-419',  # Latin American Spanish
    }
    
    espeak_lang = lang_map.get(lang, lang)
    
    # Determine number of workers
    if num_workers is None:
        num_workers = max(1, cpu_count() - 1)  # Leave one core free
    
    print(f"Generating {len(words)} pronunciations for {espeak_lang}...")
    print(f"  Using phonemizer with {num_workers} parallel jobs...")
    print(f"  This may take 1-2 minutes...")
    
    pronunciations = {}
    
    # Process in batches to show progress (phonemizer handles parallelization internally)
    total_words = len(words)
    processed = 0
    
    for i in range(0, len(words), batch_size):
        batch = words[i:i+batch_size]
        
        try:
            # phonemizer handles parallel processing internally with njobs parameter
            ipa_results = phonemize(
                batch,
                language=espeak_lang,
                backend='espeak',
                strip=True,
                preserve_punctuation=False,
                with_stress=True,
                njobs=num_workers  # Use phonemizer's built-in parallelization
            )
            
            # phonemizer returns a list of IPA strings matching input words
            if isinstance(ipa_results, list):
                for word, ipa in zip(batch, ipa_results):
                    if ipa and ipa.strip():
                        pronunciations[word] = ipa.strip()
            elif isinstance(ipa_results, str):
                # Fallback: split by newlines if returned as string
                ipa_lines = ipa_results.split('\n')
                for word, ipa in zip(batch, ipa_lines):
                    if ipa and ipa.strip():
                        pronunciations[word] = ipa.strip()
            
            processed += len(batch)
            progress_pct = (processed / total_words) * 100
            
            # Show progress more frequently at first, then every 5% 
            if processed <= 5000 or processed % 5000 == 0:
                print(f"  Processed {processed}/{total_words} words ({progress_pct:.1f}%)...", flush=True)
            
        except KeyboardInterrupt:
            print("\n\n⚠️  Interrupted by user. Stopping...", file=sys.stderr)
            break
        except Exception as e:
            print(f"  WARNING: Error in batch at {i}: {e}", file=sys.stderr)
            # Try smaller batch or individual words
            for word in batch:
                try:
                    ipa = phonemize(word, language=espeak_lang, backend='espeak', 
                                   strip=True, preserve_punctuation=False, with_stress=True)
                    if ipa and ipa.strip():
                        pronunciations[word] = ipa.strip()
                except:
                    pass
    
    print(f"Generated {len(pronunciations)} pronunciations")
    return pronunciations

def read_dictionary(input_path):
    """Read existing dictionary file and return pronunciations dict."""
    pronunciations = {}
    input_path = Path(input_path)
    
    if not input_path.exists():
        return None
    
    with open(input_path, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            
            parts = line.split('\t')
            if len(parts) == 2:
                word, ipa = parts
                pronunciations[word] = ipa
    
    print(f"📖 Read {len(pronunciations)} entries from {input_path}")
    return pronunciations

def write_dictionary(pronunciations, output_path):
    """Write dictionary in our format: WORD<tab>IPA."""
    output_path = Path(output_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    
    with open(output_path, 'w', encoding='utf-8') as f:
        f.write("# espeak-generated pronunciation dictionary\n")
        f.write(f"# Generated with espeak-ng for offline training\n")
        f.write(f"# Format: WORD<tab>IPA_PRONUNCIATION\n\n")
        
        for word in sorted(pronunciations.keys()):
            ipa = pronunciations[word]
            f.write(f"{word}\t{ipa}\n")
    
    print(f"✅ Wrote {len(pronunciations)} entries to {output_path}")
    print(f"   File size: {output_path.stat().st_size / 1024:.1f} KB")

def generate_c_header(pronunciations, output_path, lang):
    """Generate C header with embedded dictionary data."""
    output_path = Path(output_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    
    # Generate variable name from language
    var_name = lang.replace('-', '_').replace('.', '_')
    guard_name = var_name.upper()
    
    # Sort dictionary for binary search
    sorted_words = sorted(pronunciations.keys(), key=str.lower)
    
    with open(output_path, 'w', encoding='utf-8') as f:
        f.write(f"""/**
 * @file espeak_dict_{var_name}.h
 * @brief Espeak-trained pronunciation dictionary for {lang}
 * 
 * AUTO-GENERATED - DO NOT EDIT
 * Generated using espeak-ng for offline training.
 * 
 * This is training data derived from espeak-ng output, NOT espeak source code.
 * Dictionary is sorted alphabetically (case-insensitive) for binary search.
 * 
 * Generation date: {Path(__file__).stat().st_mtime}
 * Dictionary size: {len(pronunciations)} entries
 */

#ifndef ESPEAK_DICT_{guard_name}_H
#define ESPEAK_DICT_{guard_name}_H

#include "../espeak_dict.h"

#ifdef __cplusplus
extern "C" {{
#endif

#define ESPEAK_DICT_{guard_name}_ENABLED 1

const espeak_dict_entry_t espeak_dict_{var_name}[] = {{
""")
        
        for word in sorted_words:
            ipa = pronunciations[word].replace('"', '\\"').replace('\\', '\\\\')
            word_escaped = word.replace('"', '\\"').replace('\\', '\\\\')
            f.write(f'    {{"{word_escaped}", "{ipa}"}},\n')
        
        f.write(f"""    {{NULL, NULL}}  // Sentinel
}};

const size_t espeak_dict_{var_name}_size = {len(pronunciations)};

#ifdef __cplusplus
}}
#endif

#endif  // ESPEAK_DICT_{guard_name}_H
""")
    
    print(f"✅ Wrote C header to {output_path}")
    print(f"   Dictionary size: {len(pronunciations)} entries (sorted)")
    print(f"   Binary size estimate: ~{len(pronunciations) * 40 / 1024:.1f} KB")

def main():
    # Setup signal handler for main process
    signal.signal(signal.SIGINT, _signal_handler)
    
    parser = argparse.ArgumentParser(
        description='Generate espeak-compatible pronunciation dictionary',
        epilog='Example: python3 generate_espeak_dict.py --lang en-us --output data/espeak_en_us.dict'
    )
    parser.add_argument('--lang', required=True, 
                       choices=['en-us', 'en-gb', 'de', 'zh', 'es-mx'],
                       help='Target language')
    parser.add_argument('--output', required=True,
                       help='Output dictionary file path')
    parser.add_argument('--format', choices=['dict', 'header'], default='dict',
                       help='Output format: dict (text) or header (C header)')
    parser.add_argument('--vocab-file', 
                       help='Additional vocabulary file (one word per line)')
    parser.add_argument('--workers', type=int, default=None,
                       help='Number of parallel workers (default: CPU count - 1)')
    parser.add_argument('--dict-file',
                       help='Read from existing .dict file instead of regenerating')
    
    args = parser.parse_args()
    
    # Check if we should read from existing dict file
    if args.dict_file:
        pronunciations = read_dictionary(args.dict_file)
        if pronunciations is None:
            print(f"❌ Dictionary file not found: {args.dict_file}", file=sys.stderr)
            sys.exit(1)
    else:
        # Load vocabulary
        vocab = load_vocabulary(args.lang)
        
        # Add custom vocabulary if provided
        if args.vocab_file:
            with open(args.vocab_file, 'r', encoding='utf-8') as f:
                custom_words = [line.strip() for line in f if line.strip()]
                vocab.extend(custom_words)
                print(f"Added {len(custom_words)} custom words")
        
        # Generate pronunciations using espeak
        pronunciations = generate_pronunciations(vocab, args.lang, num_workers=args.workers)
    
    # Write output
    if args.format == 'header':
        # Also generate .dict file alongside header
        dict_path = Path(args.output).with_suffix('.dict')
        write_dictionary(pronunciations, dict_path)
        generate_c_header(pronunciations, args.output, args.lang)
    else:
        write_dictionary(pronunciations, args.output)
    
    print("\n✅ Dictionary generation complete!")
    print(f"   Use this data in your phonemizer for fast, espeak-compatible lookups")

if __name__ == '__main__':
    main()
