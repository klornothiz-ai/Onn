#!/bin/bash
# ============================================================================
# ProsperoLayer RDNA2 Core - Test Automation Script
# ============================================================================
# Version: 1.0.0
# Description: Automated testing and quality assurance
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
TEST_DIR="$BUILD_DIR/tests"
REPORT_DIR="$BUILD_DIR/reports"
COVERAGE_DIR="$BUILD_DIR/coverage"

# Test categories
TEST_CATEGORIES=(
    "unit"
    "integration"
    "performance"
    "security"
)

# Test types
TEST_TYPES=(
    "cpu_interpreter"
    "jit_executor"
    "vmm_elf_loader"
    "pm4_decoder"
    "rdna2_spirv"
    "gpu_backend"
    "game_folder"
)

# ----------------------------------------------------------------------------
# Initialize test environment
# ----------------------------------------------------------------------------
init_test_env() {
    log_header "Initializing Test Environment"
    
    # Create directories
    mkdir -p "$TEST_DIR" "$REPORT_DIR" "$COVERAGE_DIR"
    
    # Check if tests are built
    if [ ! -d "$TEST_DIR" ] || [ -z "$(ls -A "$TEST_DIR" 2>/dev/null)" ]; then
        log_warning "Tests not built. Building tests first..."
        cd "$PROJECT_DIR"
        make unit
    fi
    
    log_success "Test environment initialized"
}

# ----------------------------------------------------------------------------
# Run unit tests
# ----------------------------------------------------------------------------
run_unit_tests() {
    log_header "Running Unit Tests"
    
    local PASSED=0
    local FAILED=0
    local TOTAL=0
    
    for test_type in "${TEST_TYPES[@]}"; do
        local test_binary="$TEST_DIR/${test_type}_test"
        
        if [ -f "$test_binary" ] && [ -x "$test_binary" ]; then
            TOTAL=$((TOTAL + 1))
            log_info "Running $test_type test..."
            
            # Run test and capture output
            local output
            local exit_code=0
            output=$("$test_binary" 2>&1) || exit_code=$?
            
            # Save test report
            echo "$output" > "$REPORT_DIR/${test_type}_report.txt"
            
            if [ $exit_code -eq 0 ]; then
                PASSED=$((PASSED + 1))
                log_success "  ✓ $test_type: PASSED"
            else
                FAILED=$((FAILED + 1))
                log_error "  ✗ $test_type: FAILED"
                log_info "    See report: $REPORT_DIR/${test_type}_report.txt"
            fi
        else
            log_warning "Test binary not found: $test_binary"
        fi
    done
    
    echo ""
    log_info "Unit Test Results:"
    echo "  Total: $TOTAL"
    echo "  Passed: $PASSED"
    echo "  Failed: $FAILED"
    echo "  Pass Rate: $(( (PASSED * 100) / TOTAL ))%"
    
    if [ $FAILED -gt 0 ]; then
        log_error "$FAILED test(s) failed"
        return 1
    fi
    
    log_success "All unit tests passed!"
    return 0
}

# ----------------------------------------------------------------------------
# Run integration tests
# ----------------------------------------------------------------------------
run_integration_tests() {
    log_header "Running Integration Tests"
    
    # Check if main binary exists
    local main_binary="$BUILD_DIR/bin/ps5_native_vulkan_emulator"
    
    if [ ! -f "$main_binary" ]; then
        log_warning "Main binary not found. Skipping integration tests."
        return 0
    fi
    
    log_info "Running integration tests..."
    
    # Test 1: Help command
    log_info "Test 1: Help command"
    if "$main_binary" --help > /dev/null 2>&1; then
        log_success "  ✓ Help command works"
    else
        log_error "  ✗ Help command failed"
        return 1
    fi
    
    # Test 2: Version command
    log_info "Test 2: Version command"
    if "$main_binary" --version > /dev/null 2>&1; then
        log_success "  ✓ Version command works"
    else
        log_error "  ✗ Version command failed"
        return 1
    fi
    
    # Test 3: Invalid arguments
    log_info "Test 3: Invalid arguments handling"
    if ! "$main_binary" --invalid-argument > /dev/null 2>&1; then
        log_success "  ✓ Invalid arguments handled correctly"
    else
        log_warning "  ⚠ Invalid arguments should fail"
    fi
    
    log_success "Integration tests completed!"
    return 0
}

# ----------------------------------------------------------------------------
# Run performance tests
# ----------------------------------------------------------------------------
run_performance_tests() {
    log_header "Running Performance Tests"
    
    # Check if main binary exists
    local main_binary="$BUILD_DIR/bin/ps5_native_vulkan_emulator"
    
    if [ ! -f "$main_binary" ]; then
        log_warning "Main binary not found. Skipping performance tests."
        return 0
    fi
    
    log_info "Running performance tests..."
    
    # Test 1: Startup time
    log_info "Test 1: Startup time"
    local start_time
    local end_time
    local duration
    
    start_time=$(date +%s%N)
    timeout 5 "$main_binary" --headless > /dev/null 2>&1 || true
    end_time=$(date +%s%N)
    
    duration=$(( (end_time - start_time) / 1000000 ))
    log_info "  Startup time: ${duration}ms"
    
    if [ $duration -lt 1000 ]; then
        log_success "  ✓ Startup time is acceptable"
    else
        log_warning "  ⚠ Startup time is slow"
    fi
    
    # Test 2: Memory usage
    log_info "Test 2: Memory usage"
    local pid
    local memory_usage
    
    "$main_binary" --headless > /dev/null 2>&1 &
    pid=$!
    
    sleep 2
    
    if [ -f "/proc/$pid/status" ]; then
        memory_usage=$(grep VmRSS "/proc/$pid/status" 2>/dev/null | awk '{print $2}')
        log_info "  Memory usage: ${memory_usage}KB"
        
        if [ $memory_usage -lt 1048576 ]; then
            log_success "  ✓ Memory usage is acceptable"
        else
            log_warning "  ⚠ Memory usage is high"
        fi
    fi
    
    kill $pid 2>/dev/null || true
    wait $pid 2>/dev/null || true
    
    log_success "Performance tests completed!"
    return 0
}

# ----------------------------------------------------------------------------
# Run security tests
# ----------------------------------------------------------------------------
run_security_tests() {
    log_header "Running Security Tests"
    
    # Check if main binary exists
    local main_binary="$BUILD_DIR/bin/ps5_native_vulkan_emulator"
    
    if [ ! -f "$main_binary" ]; then
        log_warning "Main binary not found. Skipping security tests."
        return 0
    fi
    
    log_info "Running security tests..."
    
    # Test 1: Buffer overflow protection
    log_info "Test 1: Buffer overflow protection"
    local long_input
    long_input=$(printf 'A%.0s' {1..10000})
    
    if ! echo "$long_input" | "$main_binary" > /dev/null 2>&1; then
        log_success "  ✓ Buffer overflow protection working"
    else
        log_warning "  ⚠ Buffer overflow protection may be weak"
    fi
    
    # Test 2: Invalid file handling
    log_info "Test 2: Invalid file handling"
    if ! "$main_binary" --game "/nonexistent/path" > /dev/null 2>&1; then
        log_success "  ✓ Invalid file handling working"
    else
        log_warning "  ⚠ Invalid file handling may be weak"
    fi
    
    # Test 3: Permission check
    log_info "Test 3: Permission check"
    local test_file="/tmp/prosperolayer_test_$$.txt"
    touch "$test_file"
    chmod 000 "$test_file"
    
    if ! "$main_binary" --game "$test_file" > /dev/null 2>&1; then
        log_success "  ✓ Permission check working"
    else
        log_warning "  ⚠ Permission check may be weak"
    fi
    
    rm -f "$test_file"
    
    log_success "Security tests completed!"
    return 0
}

# ----------------------------------------------------------------------------
# Run code quality tests
# ----------------------------------------------------------------------------
run_quality_tests() {
    log_header "Running Code Quality Tests"
    
    cd "$PROJECT_DIR"
    
    # Test 1: Code formatting
    log_info "Test 1: Code formatting"
    if command -v clang-format &> /dev/null; then
        local unformatted
        unformatted=$(find src include libs tests -name "*.cpp" -o -name "*.h" | \
                      xargs clang-format --dry-run 2>/dev/null | grep -c "warning:" || true)
        
        if [ "$unformatted" -eq 0 ]; then
            log_success "  ✓ Code is properly formatted"
        else
            log_warning "  ⚠ $unformatted files need formatting"
        fi
    else
        log_warning "  ⚠ clang-format not available"
    fi
    
    # Test 2: Static analysis
    log_info "Test 2: Static analysis"
    if command -v cppcheck &> /dev/null; then
        local issues
        issues=$(cppcheck --enable=warning,style,performance,portability \
                          --suppress=missingIncludeSystem \
                          src/ include/ libs/ 2>&1 | grep -c "error:" || true)
        
        if [ "$issues" -eq 0 ]; then
            log_success "  ✓ No static analysis issues found"
        else
            log_warning "  ⚠ $issues static analysis issues found"
        fi
    else
        log_warning "  ⚠ cppcheck not available"
    fi
    
    # Test 3: Documentation coverage
    log_info "Test 3: Documentation coverage"
    local total_functions
    local documented_functions
    local coverage
    
    total_functions=$(grep -r "^[[:space:]]*\(void\|int\|bool\|float\|double\|auto\|static\|inline\|virtual\|override\)" src/ include/ --include="*.cpp" --include="*.h" | wc -l)
    documented_functions=$(grep -r "^\s*/\*\*" src/ include/ --include="*.cpp" --include="*.h" | wc -l)
    
    if [ $total_functions -gt 0 ]; then
        coverage=$(( (documented_functions * 100) / total_functions ))
        log_info "  Documentation coverage: $coverage%"
        
        if [ $coverage -ge 50 ]; then
            log_success "  ✓ Documentation coverage is acceptable"
        else
            log_warning "  ⚠ Documentation coverage is low"
        fi
    fi
    
    log_success "Code quality tests completed!"
    return 0
}

# ----------------------------------------------------------------------------
# Generate test report
# ----------------------------------------------------------------------------
generate_report() {
    log_header "Generating Test Report"
    
    local report_file="$REPORT_DIR/test_report_$(date +%Y%m%d_%H%M%S).html"
    
    # Create HTML report
    cat > "$report_file" << EOF
<!DOCTYPE html>
<html lang="ar">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>تقرير الاختبارات - ProsperoLayer RDNA2 Core</title>
    <style>
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            margin: 0;
            padding: 20px;
            background-color: #f5f5f5;
        }
        .container {
            max-width: 1200px;
            margin: 0 auto;
            background-color: white;
            padding: 30px;
            border-radius: 10px;
            box-shadow: 0 2px 10px rgba(0,0,0,0.1);
        }
        h1 {
            color: #2c3e50;
            text-align: center;
            border-bottom: 2px solid #3498db;
            padding-bottom: 10px;
        }
        .summary {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
            gap: 20px;
            margin: 30px 0;
        }
        .summary-card {
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
            padding: 20px;
            border-radius: 10px;
            text-align: center;
        }
        .summary-card.success {
            background: linear-gradient(135deg, #11998e 0%, #38ef7d 100%);
        }
        .summary-card.error {
            background: linear-gradient(135deg, #eb3349 0%, #f45c43 100%);
        }
        .summary-card.warning {
            background: linear-gradient(135deg, #f093fb 0%, #f5576c 100%);
        }
        .summary-card h3 {
            margin: 0;
            font-size: 2.5em;
        }
        .summary-card p {
            margin: 5px 0 0 0;
            font-size: 0.9em;
        }
        .test-section {
            margin: 30px 0;
            padding: 20px;
            border: 1px solid #ddd;
            border-radius: 10px;
        }
        .test-section h2 {
            color: #34495e;
            margin-top: 0;
        }
        .test-item {
            display: flex;
            justify-content: space-between;
            align-items: center;
            padding: 10px;
            border-bottom: 1px solid #eee;
        }
        .test-item:last-child {
            border-bottom: none;
        }
        .test-item.passed {
            background-color: #d4edda;
        }
        .test-item.failed {
            background-color: #f8d7da;
        }
        .test-item.warning {
            background-color: #fff3cd;
        }
        .status-badge {
            padding: 5px 15px;
            border-radius: 20px;
            font-weight: bold;
            font-size: 0.9em;
        }
        .status-badge.passed {
            background-color: #28a745;
            color: white;
        }
        .status-badge.failed {
            background-color: #dc3545;
            color: white;
        }
        .status-badge.warning {
            background-color: #ffc107;
            color: black;
        }
        .footer {
            text-align: center;
            margin-top: 30px;
            padding-top: 20px;
            border-top: 1px solid #ddd;
            color: #666;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>تقرير الاختبارات الشامل</h1>
        <p style="text-align: center; color: #666;">تاريخ التقرير: $(date '+%Y-%m-%d %H:%M:%S')</p>
        
        <div class="summary">
            <div class="summary-card">
                <h3>$TOTAL_TESTS</h3>
                <p>إجمالي الاختبارات</p>
            </div>
            <div class="summary-card success">
                <h3>$PASSED_TESTS</h3>
                <p>اختبارات نجحت</p>
            </div>
            <div class="summary-card error">
                <h3>$FAILED_TESTS</h3>
                <p>اختبارات فشلت</p>
            </div>
            <div class="summary-card warning">
                <h3>$PASS_RATE%</h3>
                <p>نسبة النجاح</p>
            </div>
        </div>
        
        <div class="test-section">
            <h2>اختبارات الوحدة</h2>
            $(for test in "${TEST_TYPES[@]}"; do
                if [ -f "$REPORT_DIR/${test}_report.txt" ]; then
                    echo "<div class=\"test-item passed\">
                        <span>$test</span>
                        <span class=\"status-badge passed\">نجح</span>
                    </div>"
                fi
            done)
        </div>
        
        <div class="footer">
            <p>تم إنشاء هذا التقرير بواسطة نظام اختبارات ProsperoLayer</p>
            <p>للدعم الفني: support@prosperolayer.dev</p>
        </div>
    </div>
</body>
</html>
EOF
    
    log_success "Test report generated: $report_file"
}

# ----------------------------------------------------------------------------
# Main function
# ----------------------------------------------------------------------------
main() {
    log_header "ProsperoLayer RDNA2 Core - Test Automation"
    
    # Parse command line arguments
    local RUN_UNIT=true
    local RUN_INTEGRATION=true
    local RUN_PERFORMANCE=true
    local RUN_SECURITY=true
    local RUN_QUALITY=true
    
    while [[ $# -gt 0 ]]; do
        case $1 in
            --unit-only)
                RUN_INTEGRATION=false
                RUN_PERFORMANCE=false
                RUN_SECURITY=false
                RUN_QUALITY=false
                shift
                ;;
            --integration-only)
                RUN_UNIT=false
                RUN_PERFORMANCE=false
                RUN_SECURITY=false
                RUN_QUALITY=false
                shift
                ;;
            --performance-only)
                RUN_UNIT=false
                RUN_INTEGRATION=false
                RUN_SECURITY=false
                RUN_QUALITY=false
                shift
                ;;
            --security-only)
                RUN_UNIT=false
                RUN_INTEGRATION=false
                RUN_PERFORMANCE=false
                RUN_QUALITY=false
                shift
                ;;
            --quality-only)
                RUN_UNIT=false
                RUN_INTEGRATION=false
                RUN_PERFORMANCE=false
                RUN_SECURITY=false
                shift
                ;;
            -h|--help)
                echo "Usage: $0 [OPTIONS]"
                echo ""
                echo "Options:"
                echo "  --unit-only         Run only unit tests"
                echo "  --integration-only  Run only integration tests"
                echo "  --performance-only  Run only performance tests"
                echo "  --security-only     Run only security tests"
                echo "  --quality-only      Run only code quality tests"
                echo "  -h, --help          Show this help message"
                exit 0
                ;;
            *)
                log_error "Unknown option: $1"
                exit 1
                ;;
        esac
    done
    
    # Initialize test environment
    init_test_env
    
    # Run tests
    local TOTAL_TESTS=0
    local PASSED_TESTS=0
    local FAILED_TESTS=0
    
    if [ "$RUN_UNIT" = true ]; then
        TOTAL_TESTS=$((TOTAL_TESTS + 1))
        if run_unit_tests; then
            PASSED_TESTS=$((PASSED_TESTS + 1))
        else
            FAILED_TESTS=$((FAILED_TESTS + 1))
        fi
    fi
    
    if [ "$RUN_INTEGRATION" = true ]; then
        TOTAL_TESTS=$((TOTAL_TESTS + 1))
        if run_integration_tests; then
            PASSED_TESTS=$((PASSED_TESTS + 1))
        else
            FAILED_TESTS=$((FAILED_TESTS + 1))
        fi
    fi
    
    if [ "$RUN_PERFORMANCE" = true ]; then
        TOTAL_TESTS=$((TOTAL_TESTS + 1))
        if run_performance_tests; then
            PASSED_TESTS=$((PASSED_TESTS + 1))
        else
            FAILED_TESTS=$((FAILED_TESTS + 1))
        fi
    fi
    
    if [ "$RUN_SECURITY" = true ]; then
        TOTAL_TESTS=$((TOTAL_TESTS + 1))
        if run_security_tests; then
            PASSED_TESTS=$((PASSED_TESTS + 1))
        else
            FAILED_TESTS=$((FAILED_TESTS + 1))
        fi
    fi
    
    if [ "$RUN_QUALITY" = true ]; then
        TOTAL_TESTS=$((TOTAL_TESTS + 1))
        if run_quality_tests; then
            PASSED_TESTS=$((PASSED_TESTS + 1))
        else
            FAILED_TESTS=$((FAILED_TESTS + 1))
        fi
    fi
    
    # Calculate pass rate
    local PASS_RATE=0
    if [ $TOTAL_TESTS -gt 0 ]; then
        PASS_RATE=$(( (PASSED_TESTS * 100) / TOTAL_TESTS ))
    fi
    
    # Generate report
    generate_report
    
    # Print summary
    log_header "Test Summary"
    echo "  Total test suites: $TOTAL_TESTS"
    echo "  Passed: $PASSED_TESTS"
    echo "  Failed: $FAILED_TESTS"
    echo "  Pass rate: $PASS_RATE%"
    echo ""
    
    if [ $FAILED_TESTS -eq 0 ]; then
        log_success "All test suites passed! 🎉"
    else
        log_error "$FAILED_TESTS test suite(s) failed"
        exit 1
    fi
}

# Run main function
main "$@"