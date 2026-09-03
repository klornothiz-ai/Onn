#!/bin/bash
# ============================================================================
# ProsperoLayer RDNA2 Core - Emulator Runner Script
# ============================================================================
# Version: 1.0.0
# Description: Run the PS5/PS4 emulation prototype with various options
# ============================================================================

set -euo pipefail

# ----------------------------------------------------------------------------
# Colors and formatting
# ----------------------------------------------------------------------------
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
MAGENTA='\033[0;35m'
CYAN='\033[0;36m'
WHITE='\033[1;37m'
NC='\033[0m' # No Color

# ----------------------------------------------------------------------------
# Helper functions
# ----------------------------------------------------------------------------
log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

log_header() {
    echo ""
    echo -e "${CYAN}========================================${NC}"
    echo -e "${WHITE} $1${NC}"
    echo -e "${CYAN}========================================${NC}"
    echo ""
}

# ----------------------------------------------------------------------------
# Configuration
# ----------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR"
BUILD_DIR="$PROJECT_DIR/build"
EMULATOR_BIN="$BUILD_DIR/bin/ps5_native_vulkan_emulator"
RUNNER_BIN="$BUILD_DIR/bin/prospero-run"
TEST_DIR="$BUILD_DIR/tests"

# Default options
VERBOSE=false
DEBUG=false
WINDOWED=true
FULLSCREEN=false
VULKAN=true
SDL2=true
GAME_PATH=""
RESOLUTION="1920x1080"
VSYNC=true
FRAMERATE=60
AUDIO=true
CONTROLLER=true

# ----------------------------------------------------------------------------
# Check if emulator is built
# ----------------------------------------------------------------------------
check_emulator() {
    if [ ! -f "$EMULATOR_BIN" ]; then
        log_error "Emulator not built. Please build first:"
        echo "  ./build.sh release"
        exit 1
    fi
    
    if [ ! -x "$EMULATOR_BIN" ]; then
        log_error "Emulator binary not executable"
        exit 1
    fi
}

# ----------------------------------------------------------------------------
# Run emulator
# ----------------------------------------------------------------------------
run_emulator() {
    log_header "Running ProsperoLayer Emulator"
    
    # Build command line
    local CMD=("$EMULATOR_BIN")
    
    # Add options
    if [ "$VERBOSE" = true ]; then
        CMD+=("--verbose")
    fi
    
    if [ "$DEBUG" = true ]; then
        CMD+=("--debug")
    fi
    
    if [ "$WINDOWED" = true ]; then
        CMD+=("--windowed")
    fi
    
    if [ "$FULLSCREEN" = true ]; then
        CMD+=("--fullscreen")
    fi
    
    if [ "$VULKAN" = true ]; then
        CMD+=("--vulkan")
    fi
    
    if [ "$SDL2" = true ]; then
        CMD+=("--sdl2")
    fi
    
    if [ -n "$GAME_PATH" ]; then
        CMD+=("--game" "$GAME_PATH")
    fi
    
    CMD+=("--resolution" "$RESOLUTION")
    
    if [ "$VSYNC" = true ]; then
        CMD+=("--vsync")
    else
        CMD+=("--no-vsync")
    fi
    
    CMD+=("--framerate" "$FRAMERATE")
    
    if [ "$AUDIO" = true ]; then
        CMD+=("--audio")
    else
        CMD+=("--no-audio")
    fi
    
    if [ "$CONTROLLER" = true ]; then
        CMD+=("--controller")
    else
        CMD+=("--no-controller")
    fi
    
    # Print command
    log_info "Executing: ${CMD[*]}"
    
    # Run emulator
    "${CMD[@]}"
}

# ----------------------------------------------------------------------------
# Run with debugger
# ----------------------------------------------------------------------------
run_debugger() {
    log_header "Running Emulator with GDB"
    
    if ! command -v gdb &> /dev/null; then
        log_error "GDB not found. Please install gdb."
        exit 1
    fi
    
    local CMD=("gdb" "--args" "$EMULATOR_BIN")
    
    if [ -n "$GAME_PATH" ]; then
        CMD+=("--game" "$GAME_PATH")
    fi
    
    log_info "Starting GDB session..."
    "${CMD[@]}"
}

# ----------------------------------------------------------------------------
# Run with valgrind
# ----------------------------------------------------------------------------
run_valgrind() {
    log_header "Running Emulator with Valgrind"
    
    if ! command -v valgrind &> /dev/null; then
        log_error "Valgrind not found. Please install valgrind."
        exit 1
    fi
    
    local CMD=("valgrind" "--leak-check=full" "--show-leak-kinds=all" "--track-origins=yes")
    
    if [ -n "$GAME_PATH" ]; then
        CMD+=("$EMULATOR_BIN" "--game" "$GAME_PATH")
    else
        CMD+=("$EMULATOR_BIN")
    fi
    
    log_info "Starting Valgrind analysis..."
    "${CMD[@]}"
}

# ----------------------------------------------------------------------------
# Run with perf
# ----------------------------------------------------------------------------
run_perf() {
    log_header "Running Emulator with Perf"
    
    if ! command -v perf &> /dev/null; then
        log_error "Perf not found. Please install linux-tools."
        exit 1
    fi
    
    local CMD=("perf" "record" "-g")
    
    if [ -n "$GAME_PATH" ]; then
        CMD+=("$EMULATOR_BIN" "--game" "$GAME_PATH")
    else
        CMD+=("$EMULATOR_BIN")
    fi
    
    log_info "Starting Perf profiling..."
    "${CMD[@]}"
    
    log_info "Generating perf report..."
    perf report
}

# ----------------------------------------------------------------------------
# Run tests
# ----------------------------------------------------------------------------
run_tests() {
    log_header "Running Tests"
    
    if [ ! -d "$TEST_DIR" ]; then
        log_error "Test directory not found. Please build tests first."
        exit 1
    fi
    
    local FAILED=0
    local TOTAL=0
    
    for test in "$TEST_DIR"/*_test; do
        if [ -x "$test" ]; then
            TOTAL=$((TOTAL + 1))
            log_info "Running $(basename "$test")..."
            
            if "$test"; then
                log_success "  ✓ Passed"
            else
                log_error "  ✗ Failed"
                FAILED=$((FAILED + 1))
            fi
        fi
    done
    
    echo ""
    log_info "Test Results: $((TOTAL - FAILED))/$TOTAL passed"
    
    if [ $FAILED -gt 0 ]; then
        log_error "$FAILED test(s) failed"
        exit 1
    fi
    
    log_success "All tests passed!"
}

# ----------------------------------------------------------------------------
# Run runner
# ----------------------------------------------------------------------------
run_runner() {
    log_header "Running Prospero Runner"
    
    if [ ! -f "$RUNNER_BIN" ]; then
        log_error "Runner not built. Please build first."
        exit 1
    fi
    
    if [ -z "$GAME_PATH" ]; then
        log_error "Game path required for runner"
        exit 1
    fi
    
    local CMD=("$RUNNER_BIN" "$GAME_PATH")
    
    if [ "$VERBOSE" = true ]; then
        CMD+=("--verbose")
    fi
    
    log_info "Executing: ${CMD[*]}"
    "${CMD[@]}"
}

# ----------------------------------------------------------------------------
# Show help
# ----------------------------------------------------------------------------
show_help() {
    echo "Usage: $0 [COMMAND] [OPTIONS]"
    echo ""
    echo "Commands:"
    echo "  run              Run the emulator (default)"
    echo "  debug            Run with GDB debugger"
    echo "  valgrind         Run with Valgrind memory checker"
    echo "  perf             Run with Perf profiler"
    echo "  test             Run all tests"
    echo "  runner           Run the ELF runner"
    echo "  help             Show this help message"
    echo ""
    echo "Options:"
    echo "  -v, --verbose    Enable verbose output"
    echo "  -d, --debug      Enable debug mode"
    echo "  -w, --windowed   Run in windowed mode"
    echo "  -f, --fullscreen Run in fullscreen mode"
    echo "  -g, --game PATH  Path to game directory"
    echo "  -r, --resolution RES  Set resolution (default: 1920x1080)"
    echo "  --vsync          Enable VSync (default: true)"
    echo "  --no-vsync       Disable VSync"
    echo "  --framerate FPS  Set framerate (default: 60)"
    echo "  --audio          Enable audio (default: true)"
    echo "  --no-audio       Disable audio"
    echo "  --controller     Enable controller (default: true)"
    echo "  --no-controller  Disable controller"
    echo "  -h, --help       Show this help message"
    echo ""
    echo "Examples:"
    echo "  $0 run --game /path/to/game"
    echo "  $0 debug --game /path/to/game"
    echo "  $0 valgrind --game /path/to/game"
    echo "  $0 test"
    echo "  $0 run --windowed --resolution 1280x720"
    echo "  $0 run --fullscreen --no-vsync --framerate 120"
}

# ----------------------------------------------------------------------------
# Main function
# ----------------------------------------------------------------------------
main() {
    # Parse command
    local COMMAND="run"
    
    if [[ $# -gt 0 ]] && [[ ! "$1" =~ ^- ]]; then
        COMMAND="$1"
        shift
    fi
    
    # Parse options
    while [[ $# -gt 0 ]]; do
        case $1 in
            -v|--verbose)
                VERBOSE=true
                shift
                ;;
            -d|--debug)
                DEBUG=true
                shift
                ;;
            -w|--windowed)
                WINDOWED=true
                FULLSCREEN=false
                shift
                ;;
            -f|--fullscreen)
                FULLSCREEN=true
                WINDOWED=false
                shift
                ;;
            -g|--game)
                GAME_PATH="$2"
                shift 2
                ;;
            -r|--resolution)
                RESOLUTION="$2"
                shift 2
                ;;
            --vsync)
                VSYNC=true
                shift
                ;;
            --no-vsync)
                VSYNC=false
                shift
                ;;
            --framerate)
                FRAMERATE="$2"
                shift 2
                ;;
            --audio)
                AUDIO=true
                shift
                ;;
            --no-audio)
                AUDIO=false
                shift
                ;;
            --controller)
                CONTROLLER=true
                shift
                ;;
            --no-controller)
                CONTROLLER=false
                shift
                ;;
            -h|--help)
                show_help
                exit 0
                ;;
            *)
                log_error "Unknown option: $1"
                show_help
                exit 1
                ;;
        esac
    done
    
    # Execute command
    case "$COMMAND" in
        run)
            check_emulator
            run_emulator
            ;;
        debug)
            check_emulator
            run_debugger
            ;;
        valgrind)
            check_emulator
            run_valgrind
            ;;
        perf)
            check_emulator
            run_perf
            ;;
        test)
            run_tests
            ;;
        runner)
            run_runner
            ;;
        help)
            show_help
            ;;
        *)
            log_error "Unknown command: $COMMAND"
            show_help
            exit 1
            ;;
    esac
}

# Run main function
main "$@"