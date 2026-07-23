#!/bin/bash

ST_DIR="st_files/PLC_Programs"
COMPILE_SCRIPT="./scripts/compile_program.sh"
OPENPLC_CMD="./core/openplc"
LOG_DIR="my_logs"
TIMESTAMP=$(date +"%Y%m%d_%H%M%S")

# Create log directory
mkdir -p "$LOG_DIR"

# Log files
SUCCESS_LOG="$LOG_DIR/success_$TIMESTAMP.log"
FAILED_LOG="$LOG_DIR/failed_$TIMESTAMP.log"
FULL_LOG="$LOG_DIR/full_$TIMESTAMP.log"
DETAIL_LOG="$LOG_DIR/detail_$TIMESTAMP.log"
CFI_STATS_LOG="$LOG_DIR/cfi_statistics_$TIMESTAMP.log"

CSV_REPORT="$LOG_DIR/report_$TIMESTAMP.csv"

echo "Starting batch processing at $(date)" | tee -a "$FULL_LOG"
echo "Success log: $SUCCESS_LOG"
echo "Failed log: $FAILED_LOG"
echo "Full log: $FULL_LOG"
echo ""

echo "File name, number of CFI-PROLOGUE in POUS. c, number of CFI-PROLOGUE in Res0. c, number of CFI-PROLOGUE in Config. c, total, compile status, run status" > "$CSV_REPORT"

count_cfi_prologue() {
    local file_path="$1"
    if [ -f "$file_path" ]; then
        grep -o "CFI_PROLOGUE_TAG;" "$file_path" | wc -l
    else
        echo "0"
    fi
}


shopt -s nullglob
st_files=("$ST_DIR"/*.ST)
total=${#st_files[@]}

if [ $total -eq 0 ]; then
    echo "error: $ST_DIR has no .st file"
    exit 1
fi

echo "find $total .st file" | tee -a "$DETAIL_LOG"
echo "" | tee -a "$DETAIL_LOG"

# Counter
total=0
success=0
failed=0

for st_file in "${st_files[@]}"; do
    
    total=$((total + 1))
    rel_path="${st_file#st_files/}"
    filename=$(basename "$st_file")
    MAIN_FILE="core/main.cpp"
    
    echo "Processing [$total]: $filename" | tee -a "$FULL_LOG"
    
    # Compile
    if $COMPILE_SCRIPT "$rel_path" >> "$FULL_LOG" 2>&1; then
        echo "  ✓ Compilation OK" | tee -a "$FULL_LOG"
        compile_status="success"
        echo "count CFI_PROLOGUE..." | tee -a "$DETAIL_LOG"
        POUS_FILE="core/POUS.c"
        RES0_FILE="core/Res0.c"
        CONFIG0_FILE="core/Config0.c"

        pous_count=$(count_cfi_prologue "$POUS_FILE")
        res0_count=$(count_cfi_prologue "$RES0_FILE")
        config0_count=$(count_cfi_prologue "$CONFIG0_FILE")
        total_cfi=$((pous_count + res0_count + config0_count))

        echo "  POUS.c: $pous_count CFI_PROLOGUE();" | tee -a "$DETAIL_LOG"
        echo "  Res0.c: $res0_count CFI_PROLOGUE();" | tee -a "$DETAIL_LOG"
        echo "  Config0.c: $config0_count CFI_PROLOGUE();" | tee -a "$DETAIL_LOG"
        echo "  Totally: $total_cfi" | tee -a "$DETAIL_LOG"


        echo "[$rel_path] - POUS:$pous_count, Res0:$res0_count, Config0:$config0_count, totally:$total_cfi" >> "$CFI_STATS_LOG"
	# sudo pkill tee-supplicant
	# sudo tee-supplicant -d
        # Run openplc (note: this might need to run in background or with timeout)
        # Add timeout to prevent hanging (adjust 600 seconds as needed)
        stdbuf -oL -eL timeout 600 $OPENPLC_CMD >> "$FULL_LOG" 2>&1
	exit_code=$?
	if [ $exit_code -ne 124 ]; then
            echo "  ✓ OpenPLC OK" | tee -a "$FULL_LOG"
            echo "$rel_path" >> "$SUCCESS_LOG"
            success=$((success + 1))
            run_status="success"
	    mv "my_logs/cycle_timing.csv" "my_logs/cycle_timing_${filename}.csv"
        else
            echo "  ✗ OpenPLC failed" | tee -a "$FULL_LOG"
            echo "$rel_path - OpenPLC failed" >> "$FAILED_LOG"
            failed=$((failed + 1))
            run_status="failed"
        fi
    else
        echo "  ✗ Compilation failed" | tee -a "$FULL_LOG"
        echo "$rel_path - Compilation failed" >> "$FAILED_LOG"
        failed=$((failed + 1))
	compile_status="failed"
    fi
    
    echo "" | tee -a "$FULL_LOG"
    echo "\"$filename\",$pous_count,$res0_count,$config0_count,$total_cfi,$compile_status,$run_status" >> "$CSV_REPORT"
done

# Summary
echo "=========================================" | tee -a "$FULL_LOG"
echo "BATCH PROCESSING COMPLETED" | tee -a "$FULL_LOG"
echo "Total: $total" | tee -a "$FULL_LOG"
echo "Success: $success" | tee -a "$FULL_LOG"
echo "Failed: $failed" | tee -a "$FULL_LOG"
echo "Logs saved in $LOG_DIR" | tee -a "$FULL_LOG"
echo "=========================================" | tee -a "$FULL_LOG"

