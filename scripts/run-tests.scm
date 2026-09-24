#!/usr/bin/env csi -s
;;; NES Test Suite Runner (Chicken Scheme)
;;;
;;; Single entry point for all test orchestration.
;;;
;;; Usage:
;;;   csi -s scripts/run-tests.scm all          Run everything
;;;   csi -s scripts/run-tests.scm accuracy     Run AccuracyCoin
;;;   csi -s scripts/run-tests.scm blargg       Run Blargg CPU instruction and reset tests
;;;   csi -s scripts/run-tests.scm unit         Run C unit tests (cpu, ppu, nes)
;;;   csi -s scripts/run-tests.scm quick        Run unit + accuracy (CI fast path)

(import (chicken process-context))
(load "scripts/test-lib.scm")

;;; =========================================================================
;;; Test Suites
;;; =========================================================================

(define (run-unit-tests)
  (display "=== Unit Tests ===\n\n")
  (let ((results '()))
    (for-each
     (lambda (pair)
       (let* ((name (car pair))
              (cmd (cdr pair))
              (result (run-command cmd))
              (code (car result))
              (output (cdr result)))
         (printf "  ~a... ~a\n" name (if (= code 0) "PASS" "FAIL"))
         (set! results (cons code results))))
     '(("CPU tests"     . "./build/bin/test_cpu")
       ("PPU tests"     . "./build/bin/test_ppu")
       ("NES tests"     . "./build/bin/test_nes")))
    (let ((failures (length (filter (lambda (c) (not (= c 0))) results))))
      (printf "\n  ~a/~a passed\n\n" (- (length results) failures) (length results))
      failures)))

(define (run-accuracy-coin)
  (display "=== AccuracyCoin ===\n\n")
  (let* ((result (run-command "./build/bin/accuracy_coin"))
         (code (car result))
         (output (cdr result)))
    (for-each
     (lambda (line)
       (when (or (string-contains line "PASS")
                 (string-contains line "FAIL")
                 (string-contains line "Summary"))
         (printf "  ~a\n" line)))
     (string-split output "\n"))
    (newline)
    code))

(define (run-blargg-suite)
  (display "=== Blargg CPU Instruction Tests ===\n\n")
  (reset-counts!)
  (let ((rom-dir "tests/nes-test-roms/instr_test-v5/rom_singles"))
    (for-each
     (lambda (name)
       (test-rom name (string-append rom-dir "/" name ".nes")))
     '("01-basics" "02-implied" "03-immediate" "04-zero_page"
       "05-zp_xy" "06-absolute" "07-abs_xy" "08-ind_x"
       "09-ind_y" "10-branches" "11-stack" "12-jmp_jsr"
       "13-rts" "14-rti" "15-brk" "16-special")))
  (newline)
  (display "=== Blargg Reset Tests ===\n\n")
  (for-each
   (lambda (path)
     (test-rom (car (reverse (string-split path "/")))
               (string-append "tests/nes-test-roms/" path ".nes")))
   '("apu_reset/4015_cleared" "apu_reset/4017_timing" "apu_reset/4017_written"
     "apu_reset/irq_flag_cleared" "apu_reset/len_ctrs_enabled"
     "apu_reset/works_immediately" "cpu_reset/ram_after_reset"))
  (print-summary))

;;; Helpers. Stock CHICKEN 5 has no SRFI 1 without the srfi-1 egg, so the
;;; one list procedure needed is defined here.
(define (filter keep? lst)
  (let loop ((lst lst) (acc '()))
    (cond
     ((null? lst) (reverse acc))
     ((keep? (car lst)) (loop (cdr lst) (cons (car lst) acc)))
     (else (loop (cdr lst) acc)))))

(define (string-contains str sub)
  (let ((slen (string-length str))
        (sublen (string-length sub)))
    (let loop ((i 0))
      (cond
       ((> (+ i sublen) slen) #f)
       ((string=? (substring str i (+ i sublen)) sub) #t)
       (else (loop (+ i 1)))))))

;;; =========================================================================
;;; Command Dispatch
;;; =========================================================================

(define (main args)
  (let ((cmd (if (null? args) "quick" (car args))))
    (cond
     ((string=? cmd "unit")
      (exit (run-unit-tests)))
     ((string=? cmd "accuracy")
      (exit (run-accuracy-coin)))
     ((string=? cmd "blargg")
      (exit (run-blargg-suite)))
     ((string=? cmd "quick")
      (let ((f1 (run-unit-tests))
            (f2 (run-accuracy-coin)))
        (exit (if (and (= f1 0) (= f2 0)) 0 1))))
     ((string=? cmd "all")
      (let ((f1 (run-unit-tests))
            (f2 (run-accuracy-coin))
            (f3 (run-blargg-suite)))
        (exit (if (and (= f1 0) (= f2 0) (= f3 0)) 0 1))))
     (else
      (printf "Unknown command: ~a\n" cmd)
      (printf "Usage: csi -s scripts/run-tests.scm [unit|accuracy|blargg|quick|all]\n")
      (exit 1)))))

(main (command-line-arguments))
