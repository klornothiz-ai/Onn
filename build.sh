#!/bin/bash
# ============================================================================
# ProsperoLayer RDNA2 Core - Automated Build Script
# ============================================================================
# Version: 1.0.0
# Description: Automated build, test, and quality assurance script
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

log_step() {
    echo -e "${MAGENTA}[STEP]${NC} $1"
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
BUILD_TYPE="${1:-release}"
JOBS="${2:-$(nproc)}"
ENABLE_TESTS="${3:-true}"
ENABLE_FORMAT="${4:-true}"
ENABLE_ANALYSIS="${5:-true}"

# ----------------------------------------------------------------------------
# Check dependencies
# ----------------------------------------------------------------------------
check_dependencies() {
    log_header "Checking Dependencies"
    
    local missing_deps=()
    
    # Check for required tools
    if ! command -v g++ &> /dev/null; then
        missing_deps+=("g++")
    fi
    
    if ! command -v make &> /dev/null; then
        missing_deps+=("make")
    fi
    
    if ! command -v cmake &> /dev/null; then
        missing_deps+=("cmake")
    fi
    
    # Check for optional tools
    if ! command -v clang-format &> /dev/null; then
        log_warning "clang-format not found (optional)"
    fi
    
    if ! command -v clang-tidy &> /dev/null; then
        log_warning "clang-tidy not found (optional)"
    fi
    
    if ! command -v cppcheck &> /dev/null; then
        log_warning "cppcheck not found (optional)"
    fi
    
    # Report missing dependencies
    if [ ${#missing_deps[@]} -ne 0 ]; then
        log_error "Missing required dependencies: ${missing_deps[*]}"
        log_info "Please install them using your package manager"
        exit 1
    fi
    
    log_success "All required dependencies found"
}

# ----------------------------------------------------------------------------
# Clean build
# ----------------------------------------------------------------------------
clean_build() {
    log_header "Cleaning Build Directory"
    
    if [ -d "$BUILD_DIR" ]; then
        log_info "Removing existing build directory..."
        rm -rf "$BUILD_DIR"
    fi
    
    log_info "Cleaning project root..."
    cd "$PROJECT_DIR"
    make clean 2>/dev/null || true
    
    log_success "Build directory cleaned"
}

# ----------------------------------------------------------------------------
# Configure build
# ----------------------------------------------------------------------------
configure_build() {
    log_header "Configuring Build"
    
    cd "$PROJECT_DIR"
    
    # Create build directory
    mkdir -p "$BUILD_DIR"
    
    # Configure based on build type
    case "$BUILD_TYPE" in
        debug)
            log_info "Configuring debug build..."
            CMAKE_BUILD_TYPE="Debug"
            MAKE_TARGET="debug"
            ;;
        release)
            log_info "Configuring release build..."
            CMAKE_BUILD_TYPE="Release"
            MAKE_TARGET="release"
            ;;
        sanitizer)
            log_info "Configuring sanitizer build..."
            CMAKE_BUILD_TYPE="Debug"
            MAKE_TARGET="sanitize"
            ;;
        coverage)
            log_info "Configuring coverage build..."
            CMAKE_BUILD_TYPE="Debug"
            MAKE_TARGET="coverage"
            ;;
        *)
            log_error "Unknown build type: $BUILD_TYPE"
            log_info "Available types: debug, release, sanitizer, coverage"
            exit 1
            ;;
    esac
    
    log_success "Build configured for $BUILD_TYPE"
}

# ----------------------------------------------------------------------------
# Build project
# ----------------------------------------------------------------------------
build_project() {
    log_header "Building Project"
    
    cd "$PROJECT_DIR"
    
    log_info "Building with $JOBS parallel jobs..."
    
    # Build using Make
    make "$MAKE_TARGET" -j"$JOBS"
    
    log_success "Project built successfully"
}

# ----------------------------------------------------------------------------
# Run tests
# ----------------------------------------------------------------------------
run_tests() {
    if [ "$ENABLE_TESTS" != "true" ]; then
        log_info "Tests disabled, skipping..."
        return 0
    fi
    
    log_header "Running Tests"
    
    cd "$PROJECT_DIR"
    
    log_info "Running unit tests..."
    
    # Run unit tests
    make unit
    
    log_success "All tests passed"
}

# ----------------------------------------------------------------------------
# Format code
# ----------------------------------------------------------------------------
format_code() {
    if [ "$ENABLE_FORMAT" != "true" ]; then
        log_info "Code formatting disabled, skipping..."
        return 0
    fi
    
    if ! command -v clang-format &> /dev/null; then
        log_warning "clang-format not found, skipping formatting"
        return 0
    fi
    
    log_header "Formatting Code"
    
    cd "$PROJECT_DIR"
    
    log_info "Running clang-format..."
    
    # Find and format all C++ files
    find src include libs tests -name "*.cpp" -o -name "*.h" | \
        xargs -I {} clang-format -i {} 2>/dev/null || true
    
    log_success "Code formatted"
}

# ----------------------------------------------------------------------------
# Run static analysis
# ----------------------------------------------------------------------------
run_analysis() {
    if [ "$ENABLE_ANALYSIS" != "true" ]; then
        log_info "Static analysis disabled, skipping..."
        return 0
    fi
    
    log_header "Running Static Analysis"
    
    cd "$PROJECT_DIR"
    
    # Run cppcheck if available
    if command -v cppcheck &> /dev/null; then
        log_info "Running cppcheck..."
        cppcheck --enable=warning,style,performance,portability \
                 --suppress=missingIncludeSystem \
                 src/ include/ libs/ 2>&1 || true
    fi
    
    log_success "Static analysis completed"
}

# ----------------------------------------------------------------------------
# Generate documentation
# ----------------------------------------------------------------------------
generate_docs() {
    if ! command -v doxygen &> /dev/null; then
        log_warning "doxygen not found, skipping documentation generation"
        return 0
    fi
    
    log_header "Generating Documentation"
    
    cd "$PROJECT_DIR"
    
    if [ -f "Doxyfile" ]; then
        log_info "Generating API documentation..."
        doxygen Doxyfile 2>&1 || true
        log_success "Documentation generated in build/docs/"
    else
        log_warning "Doxyfile not found, skipping documentation generation"
    fi
}

# ----------------------------------------------------------------------------
# Create release package
# ----------------------------------------------------------------------------
create_package() {
    log_header "Creating Release Package"
    
    cd "$PROJECT_DIR"
    
    local PACKAGE_NAME="prosperolayer-rdna2-core-$(date +%Y%m%d)-$BUILD_TYPE"
    local PACKAGE_DIR="$BUILD_DIR/$PACKAGE_NAME"
    
    # Create package directory
    mkdir -p "$PACKAGE_DIR"/{bin,lib,share,docs}
    
    # Copy binaries
    if [ -f "build/bin/ps5_native_vulkan_emulator" ]; then
        cp "build/bin/ps5_native_vulkan_emulator" "$PACKAGE_DIR/bin/"
    fi
    
    if [ -f "build/bin/prospero-run" ]; then
        cp "build/bin/prospero-run" "$PACKAGE_DIR/bin/"
    fi
    
    # Copy documentation
    cp README.md "$PACKAGE_DIR/docs/"
    cp DEVELOPMENT_GUIDE_AR.md "$PACKAGE_DIR/docs/" 2>/dev/null || true
    
    # Create archive
    cd "$BUILD_DIR"
    tar -czf "$PACKAGE_NAME.tar.gz" "$PACKAGE_NAME"
    
    log_success "Release package created: $BUILD_DIR/$PACKAGE_NAME.tar.gz"
}

# ----------------------------------------------------------------------------
# Print summary
# ----------------------------------------------------------------------------
print_summary() {
    log_header "Build Summary"
    
    echo -e "${WHITE}Build Type:${NC} $BUILD_TYPE"
    echo -e "${WHITE}Parallel Jobs:${NC} $JOBS"
    echo -e "${WHITE}Tests:${NC} $ENABLE_TESTS"
    echo -e "${WHITE}Formatting:${NC} $ENABLE_FORMAT"
    echo -e "${WHITE}Analysis:${NC} $ENABLE_ANALYSIS"
    echo ""
    
    if [ -f "build/bin/ps5_native_vulkan_emulator" ]; then
        echo -e "${GREEN}✅ Main binary:${NC} build/bin/ps5_native_vulkan_emulator"
    fi
    
    if [ -f "build/bin/prospero-run" ]; then
        echo -e "${GREEN}✅ Runner:${NC} build/bin/prospero-run"
    fi
    
    echo ""
    echo -e "${CYAN}To run the emulator:${NC}"
    echo "  ./build/bin/ps5_native_vulkan_emulator"
    echo ""
    echo -e "${CYAN}To run tests:${NC}"
    echo "  make unit"
    echo ""
}

# ----------------------------------------------------------------------------
# Main function
# ----------------------------------------------------------------------------
main() {
    log_header "ProsperoLayer RDNA2 Core - Automated Build"
    
    # Parse command line arguments
    while [[ $# -gt 0 ]]; do
        case $1 in
            -t|--type)
                BUILD_TYPE="$2"
                shift 2
                ;;
            -j|--jobs)
                JOBS="$2"
                shift 2
                ;;
            --no-tests)
                ENABLE_TESTS="false"
                shift
                ;;
            --no-format)
                ENABLE_FORMAT="false"
                shift
                ;;
            --no-analysis)
                ENABLE_ANALYSIS="false"
                shift
                ;;
            -h|--help)
                echo "Usage: $0 [OPTIONS]"
                echo ""
                echo "Options:"
                echo "  -t, --type TYPE      Build type (debug, release, sanitizer, coverage)"
                echo "  -j, --jobs N         Number of parallel jobs (default: $(nproc))"
                echo "  --no-tests           Skip running tests"
                echo "  --no-format          Skip code formatting"
                echo "  --no-analysis        Skip static analysis"
                echo "  -h, --help           Show this help message"
                exit 0
                ;;
            *)
                log_error "Unknown option: $1"
                exit 1
                ;;
        esac
    done
    
    # Execute build steps
    check_dependencies
    clean_build
    configure_build
    build_project
    format_code
    run_tests
    run_analysis
    create_package
    print_summary
    
    log_success "Build completed successfully! 🎉"
}

# Run main function
main "$@"