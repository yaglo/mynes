;;; NES Test Library for Chicken Scheme
;;;
;;; Provides test orchestration functions that invoke test_runner
;;; and parse results. This is the single scripting layer for
;;; test automation — no Python, no other scripting languages.
;;;
;;; Usage:
;;;   (load "scripts/test-lib.scm")
;;;   (run-blargg-test "tests/nes-test-roms/instr_test-v5/rom_singles/01-basics.nes")

(import (chicken process) (chicken string) (chicken io) (chicken format)
        (chicken bitwise))

;;; Configuration
(define *test-runner* "./build/bin/test_runner")
(define *verbose* #f)

;;; Run a command and return (exit-code . output-string)
(define (run-command cmd)
  (let* ((p (open-input-pipe (string-append cmd " 2>&1")))
         (output (read-string #f p))
         (raw-status (close-input-pipe p))
         ;; On POSIX, close-input-pipe returns the raw waitpid status
         ;; Exit code is in bits 8-15
         (code (if (number? raw-status) (arithmetic-shift raw-status -8) raw-status)))
    (cons code output)))

;;; Run a Blargg test ROM. Returns 'pass, 'fail, 'timeout, or 'error.
(define (run-blargg-test rom-path #!key (frames 18000) (verbose *verbose*))
  (let* ((cmd (sprintf "~a ~a --blargg --frames ~a"
                       *test-runner* (qs rom-path) frames))
         (result (run-command cmd))
         (code (car result))
         (output (cdr result)))
    (when verbose
      (display output))
    (case code
      ((0) 'pass)
      ((1) 'fail)
      ((2) 'timeout)
      (else 'error))))

;;; Quote a shell argument
(define (qs s) (string-append "'" s "'"))

;;; Run test_runner with a script file
(define (run-script rom-path script-path #!key (extra-args ""))
  (let* ((cmd (sprintf "~a ~a --script ~a ~a"
                       *test-runner* (qs rom-path) (qs script-path) extra-args))
         (result (run-command cmd)))
    (cons (car result) (cdr result))))

;;; Run test_runner and capture screen dump
(define (run-with-dump rom-path #!key (frames 600) (charmap "data/charmaps/default.map"))
  (let* ((cmd (sprintf "~a ~a --frames ~a --dump-on-exit --charmap ~a"
                       *test-runner* (qs rom-path) frames (qs charmap)))
         (result (run-command cmd)))
    (cdr result)))  ; return just the output

;;; =========================================================================
;;; Test Suite Runner
;;; =========================================================================

(define *pass-count* 0)
(define *fail-count* 0)
(define *timeout-count* 0)
(define *skip-count* 0)

(define (reset-counts!)
  (set! *pass-count* 0)
  (set! *fail-count* 0)
  (set! *timeout-count* 0)
  (set! *skip-count* 0))

;;; Left-justify s in a field of width characters (format has no width).
(define (pad-right s width)
  (let ((len (string-length s)))
    (if (>= len width)
        s
        (string-append s (make-string (- width len) #\space)))))

(define (test-rom name rom-path #!key (frames 18000))
  (display (string-append "  " (pad-right name 40) " "))
  (flush-output)
  (let ((result (run-blargg-test rom-path frames: frames)))
    (case result
      ((pass)    (display "PASS\n")    (set! *pass-count* (+ *pass-count* 1)))
      ((fail)    (display "FAIL\n")    (set! *fail-count* (+ *fail-count* 1)))
      ((timeout) (display "TIMEOUT\n") (set! *timeout-count* (+ *timeout-count* 1)))
      ((error)   (display "SKIP\n")    (set! *skip-count* (+ *skip-count* 1))))
    result))

(define (print-summary)
  (let ((total (+ *pass-count* *fail-count* *timeout-count* *skip-count*)))
    (printf "\nResults: ~a pass, ~a fail, ~a timeout, ~a skip (of ~a)\n"
            *pass-count* *fail-count* *timeout-count* *skip-count* total)
    (if (> *fail-count* 0) 1 0)))
