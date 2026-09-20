#!/usr/bin/env csi -s
;; 6502 DSL to C compiler (Chicken Scheme)

(import (chicken base)
        (chicken io)
        (chicken format)
        (chicken process-context))

;; ============================================================================
;; Utilities
;; ============================================================================

;; Only single-list predicates are needed; keep the generator usable with a
;; stock Chicken installation, without separately installed eggs.
(define (find pred xs)
  (cond ((null? xs) #f)
        ((pred (car xs)) (car xs))
        (else (find pred (cdr xs)))))

(define (any pred xs)
  (and (pair? xs) (or (pred (car xs)) (any pred (cdr xs)))))

(define (filter pred xs)
  (cond ((null? xs) '())
        ((pred (car xs)) (cons (car xs) (filter pred (cdr xs))))
        (else (filter pred (cdr xs)))))

(define (flatmap f lst)
  (apply append (map f lst)))

(define (assoc-ref alist key)
  (let ((pair (assoc key alist)))
    (if pair (cdr pair) #f)))

;; ============================================================================
;; Global state
;; ============================================================================

(define *states* '())
(define *templates* '())
(define *cycle-macros* '())  ;; ((name . (ops...)) ...)
(define *opcodes* (make-vector 256 #f))
(define *state-base* '())

;; Per-case bus address peek table — populated by emit-state.
;; Each entry: (upc . addr-c-expr)
(define *peek-addrs* '())

;; Set of uPCs whose cycle is a pure write — DMC DMA cannot halt on these.
(define *write-cases* '())

;; Set of uPCs whose cycle is a SHA/SHX/SHY/TAS dummy-read pre-write —
;; if a DMC DMA halts here, the next write must ignore the H register
;; (AccuracyCoin SHA/SHX/SHY tests sub-test 7+, C# Emulator.cs IgnoreH).
(define *sha-fixup-cases* '())

;; ============================================================================
;; Definition collection
;; ============================================================================

(define (collect-definitions! forms)
  (for-each
   (lambda (form)
     (when (pair? form)
       (case (car form)
         ((program) (collect-definitions! (cdr form)))
         ((state)
          (let ((name (cadr form))
                (body (cddr form)))
            (set! *states* (cons (cons name body) *states*))))
         ((template)
          (let ((name (cadr form))
                (params (caddr form))
                (body (cdddr form)))
            (set! *templates* (cons (list name params body) *templates*))))
         ((defcycle)
          ;; (defcycle name ops...) - defines a reusable cycle pattern
          (let ((name (cadr form))
                (ops (cddr form)))
            (set! *cycle-macros* (cons (cons name ops) *cycle-macros*))))
         ((opcode)
          (let ((byte (cadr form))
                (state (caddr form)))
            (vector-set! *opcodes* byte state))))))
   forms))

;; ============================================================================
;; Template expansion
;; ============================================================================

(define (find-template name)
  (assoc name *templates*))

(define (find-cycle-macro name)
  (assoc name *cycle-macros*))

(define (substitute expr params args)
  (cond
   ((symbol? expr)
    (let loop ((ps params) (as args))
      (cond
       ((null? ps) expr)
       ((eq? expr (car ps)) (car as))
       (else (loop (cdr ps) (cdr as))))))
   ((pair? expr)
    (map (lambda (e) (substitute e params args)) expr))
   (else expr)))

;; Expand a single form that might be a template call (for use inside cycles)
;; Returns a list of forms (for splicing into the cycle)
(define (expand-cycle-form form)
  (cond
   ((not (pair? form)) (list form))
   ((find-template (car form))
    => (lambda (tmpl)
         (let ((params (cadr tmpl))
               (tmpl-body (caddr tmpl))
               (args (cdr form)))
           ;; Template call expands to its body forms
           (let ((expanded (map (lambda (e) (substitute e params args)) tmpl-body)))
             ;; Recursively expand any nested template calls
             (flatmap expand-cycle-form expanded)))))
   (else (list form))))

(define (expand-templates body)
  (flatmap
   (lambda (item)
     (cond
      ((not (pair? item)) (list item))
      ;; Cycle macro call - (macro-name) expands to (cycle ops...)
      ((and (null? (cdr item)) (find-cycle-macro (car item)))
       => (lambda (macro)
            (list (cons 'cycle (cdr macro)))))
      ;; Top-level template call
      ((find-template (car item))
       => (lambda (tmpl)
            (let ((params (cadr tmpl))
                  (tmpl-body (caddr tmpl))
                  (args (cdr item)))
              (expand-templates
               (map (lambda (e) (substitute e params args)) tmpl-body)))))
      ;; Cycle - expand templates and cycle macros inside it
      ((eq? (car item) 'cycle)
       (let ((expanded-forms (flatmap expand-cycle-form (cdr item))))
         (list (cons 'cycle expanded-forms))))
      ;; When block
      ((eq? (car item) 'when)
       (list (cons 'when (cons (cadr item) (expand-templates (cddr item))))))
      (else (list item))))
   body))

;; ============================================================================
;; Cycle counting
;; ============================================================================

(define (count-cycles body)
  (apply +
         (map (lambda (item)
                (cond
                 ((not (pair? item)) 0)
                 ((eq? (car item) 'cycle) 1)
                 ((eq? (car item) 'when) (count-cycles (cddr item)))
                 ((eq? (car item) 'poll-interrupts) 0)  ;; poll is embedded in first cycle, not separate
                 (else 0)))
              body)))

;; ============================================================================
;; Code generation helpers
;; ============================================================================

(define (reg->field r)
  (case r
    ((a) "cpu->A") ((x) "cpu->X") ((y) "cpu->Y")
    ((sp) "cpu->SP") ((p) "cpu->P") ((ir) "cpu->IR")
    ((dl) "cpu->DL") ((adl) "cpu->ADL") ((adh) "cpu->ADH")
    ((pcl) "(cpu->PC & 0xFF)") ((pch) "((cpu->PC >> 8) & 0xFF)")
    (else "0")))

(define (emit-set-reg port r val)
  (case r
    ((a) (fprintf port "        cpu->A = ~a;~%" val))
    ((x) (fprintf port "        cpu->X = ~a;~%" val))
    ((y) (fprintf port "        cpu->Y = ~a;~%" val))
    ((sp) (fprintf port "        cpu->SP = ~a;~%" val))
    ((p) (fprintf port "        cpu->P = ~a;~%" val))
    ((ir) (fprintf port "        cpu->IR = ~a;~%" val))
    ((dl) (fprintf port "        cpu->DL = ~a;~%" val))
    ((adl) (fprintf port "        cpu->ADL = ~a;~%" val))
    ((adh) (fprintf port "        cpu->ADH = ~a;~%" val))
    ((pcl) (fprintf port "        cpu->PC = (cpu->PC & 0xFF00) | (~a);~%" val))
    ((pch) (fprintf port "        cpu->PC = (cpu->PC & 0x00FF) | ((~a) << 8);~%" val))))

(define (addr->expr a)
  (case a
    ((pc) "cpu->PC")
    ((ad) "((uint16_t)cpu->ADH << 8) | cpu->ADL")
    ((sp) "(0x0100 | cpu->SP)")
    ((vec-reset-lo) "0xFFFC") ((vec-reset-hi) "0xFFFD")
    ((vec-nmi-lo) "0xFFFA") ((vec-nmi-hi) "0xFFFB")
    ((vec-irq-lo) "0xFFFE") ((vec-irq-hi) "0xFFFF")
    ;; NMI hijacking: use NMI vector if NMI pending during BRK/IRQ
    ((vec-irq-hijack-lo) "cpu->interrupt_vector")
    ((vec-irq-hijack-hi) "(cpu->interrupt_vector + 1)")
    (else "0")))

(define (flag->mask f)
  (case f
    ((c) "0x01") ((z) "0x02") ((i) "0x04") ((d) "0x08")
    ((b) "0x10") ((v) "0x40") ((n) "0x80")
    (else "0")))

(define (emit-cond port c)
  (cond
   ((symbol? c)
    (case c
      ((page-cross) (display "cpu->page_cross" port))
      ((branch-taken) (display "cpu->branch_taken" port))
      ((c) (display "(cpu->P & 0x01)" port))
      ((z) (display "(cpu->P & 0x02)" port))
      ((i) (display "(cpu->P & 0x04)" port))
      ((d) (display "(cpu->P & 0x08)" port))
      ((v) (display "(cpu->P & 0x40)" port))
      ((n) (display "(cpu->P & 0x80)" port))
      ((irq-pending) (display "cpu->irq_pending" port))
      ((nmi-pending) (display "cpu->nmi_pending" port))
      ((reset-pending) (display "cpu->reset_pending" port))
      (else (display "0" port))))
   ((and (pair? c) (eq? (car c) 'not))
    (display "!(" port)
    (emit-cond port (cadr c))
    (display ")" port))
   (else (display "0" port))))

(define (emit-bus-op port op)
  (case (car op)
    ((fetch)
     (fprintf port "        cpu->last_read_addr = cpu->PC;~%")
     (emit-set-reg port (cadr op) "cpu->mem_read(cpu, cpu->PC)")
     (fprintf port "        cpu->PC = (cpu->PC + 1) & 0xFFFF;~%"))
    ((read)
     (let ((addr-str (addr->expr (caddr op))))
       (fprintf port "        cpu->last_read_addr = ~a;~%" addr-str)
       (emit-set-reg port (cadr op)
                     (string-append "cpu->mem_read(cpu, " addr-str ")"))))
    ((write)
     (fprintf port "        cpu->mem_write(cpu, ~a, ~a);~%"
              (addr->expr (cadr op)) (reg->field (caddr op))))
    ((dummy)
     (let ((addr-str (addr->expr (cadr op))))
       (fprintf port "        cpu->last_read_addr = ~a;~%" addr-str)
       (fprintf port "        (void)cpu->mem_read(cpu, ~a);~%" addr-str)))))

(define (emit-internal-op port op)
  (case (car op)
    ((mov)
        (emit-set-reg port (cadr op) (reg->field (caddr op))))

    ((nz)
     (fprintf port "        { uint8_t v = ~a; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }~%"
              (reg->field (cadr op))))

    ((set-flag)
     (fprintf port "        cpu->P |= ~a;~%" (flag->mask (cadr op))))

    ((clear-flag)
     (fprintf port "        cpu->P &= ~~~a;~%" (flag->mask (cadr op))))

    ;; Snapshot the current I flag into effective_i (the value used for IRQ polling).
    ;; Place BEFORE clear-flag/set-flag/read-p to capture the OLD I (CLI/SEI/PLP).
    ;; Place AFTER read-p (RTI) or set-flag (BRK/NMI/IRQ entry) to capture the NEW I.
    ((snapshot-i)
     (fprintf port "        cpu->effective_i = (cpu->P >> 2) & 1;~%"))

    ;; Additional IRQ arming point for branches with page-cross fixup.
    ;; C# Emulator.cs calls PollInterrupts_CantDisableIRQ() at cycle 3
    ;; of branches with page crossing.
    ((arm-irq)
     (fprintf port "        if (cpu->irq_pending && !cpu->effective_i) cpu->irq_armed = 1;~%"))

    ((add8-latch-carry)
     (fprintf port "        { uint16_t s = (uint16_t)~a + (uint16_t)~a; ~a = s & 0xFF; cpu->page_cross = (s > 0xFF); }~%"
              (reg->field (caddr op)) (reg->field (cadddr op)) (reg->field (cadr op))))

    ((adc8-from-pagecross)
     (emit-set-reg port (cadr op) (string-append (reg->field (caddr op)) " + (cpu->page_cross ? 1 : 0)"))
     (fprintf port "        cpu->page_cross = 0;~%"))

    ;; SHA page fixup: on page cross, corrupt ADH to (ADH + 1) & A & X
    ;; Also pre-compute DL = A & X & H, where H = (original ADH + 1) unless
    ;; cpu->ignore_h is set (DMC DMA halt landed on this dummy read cycle),
    ;; in which case H = 0xFF — the H register drops out of the equation.
    ;; AccuracyCoin SHA tests sub-test 7+. ignore_h is cleared after use.
    ((sha-page-fixup)
     (fprintf port "        { uint8_t h1 = cpu->ignore_h ? 0xFF : (cpu->ADH + 1); cpu->ignore_h = 0; cpu->DL = cpu->A & cpu->X & h1; ")
     (fprintf port "if (cpu->page_cross) { cpu->ADH = h1 & cpu->A & cpu->X; cpu->page_cross = 0; } }~%"))

    ;; SHX page fixup: on page cross, corrupt ADH to (ADH + 1) & X
    ;; DL = X & H, where H = (ADH + 1) unless cpu->ignore_h (then 0xFF).
    ((shx-page-fixup)
     (fprintf port "        { uint8_t h1 = cpu->ignore_h ? 0xFF : (cpu->ADH + 1); cpu->ignore_h = 0; cpu->DL = cpu->X & h1; ")
     (fprintf port "if (cpu->page_cross) { cpu->ADH = h1 & cpu->X; cpu->page_cross = 0; } }~%"))

    ;; SHY page fixup: on page cross, corrupt ADH to (ADH + 1) & Y
    ;; DL = Y & H, where H = (ADH + 1) unless cpu->ignore_h (then 0xFF).
    ((shy-page-fixup)
     (fprintf port "        { uint8_t h1 = cpu->ignore_h ? 0xFF : (cpu->ADH + 1); cpu->ignore_h = 0; cpu->DL = cpu->Y & h1; ")
     (fprintf port "if (cpu->page_cross) { cpu->ADH = h1 & cpu->Y; cpu->page_cross = 0; } }~%"))

    ;; TAS page fixup: SP = A & X, DL = SP & H
    ;; H = (ADH + 1) unless cpu->ignore_h (then 0xFF).
    ((tas-page-fixup)
     (fprintf port "        { uint8_t h1 = cpu->ignore_h ? 0xFF : (cpu->ADH + 1); cpu->ignore_h = 0; cpu->SP = cpu->A & cpu->X; cpu->DL = cpu->SP & h1; ")
     (fprintf port "if (cpu->page_cross) { cpu->ADH = h1 & cpu->A & cpu->X; cpu->page_cross = 0; } }~%"))

    ((adc)
     (fprintf port "        { uint8_t a = ~a, b = ~a, c = cpu->P & 1; uint16_t s = a + b + c; uint8_t r = s & 0xFF;~%"
              (reg->field (cadr op)) (reg->field (caddr op)))
     (fprintf port "          cpu->P = (cpu->P & 0x3C) | (s > 0xFF ? 1 : 0) | (r == 0 ? 2 : 0) | (r & 0x80) | ((~~(a^b) & (a^r) & 0x80) ? 0x40 : 0);~%")
     (emit-set-reg port (cadr op) "r")
     (fprintf port "        }~%"))

    ((sbc)
     (fprintf port "        { uint8_t a = ~a, b = ~a, c = cpu->P & 1; int16_t d = a - b - (1 - c); uint8_t r = d & 0xFF;~%"
              (reg->field (cadr op)) (reg->field (caddr op)))
     (fprintf port "          cpu->P = (cpu->P & 0x3C) | (d >= 0 ? 1 : 0) | (r == 0 ? 2 : 0) | (r & 0x80) | (((a^b) & (a^r) & 0x80) ? 0x40 : 0);~%")
     (emit-set-reg port (cadr op) "r")
     (fprintf port "        }~%"))

    ((and)
     (fprintf port "        { uint8_t r = ~a & ~a; ~a = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }~%"
              (reg->field (cadr op)) (reg->field (caddr op)) (reg->field (cadr op))))

    ((ora)
     (fprintf port "        { uint8_t r = ~a | ~a; ~a = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }~%"
              (reg->field (cadr op)) (reg->field (caddr op)) (reg->field (cadr op))))

    ((eor)
     (fprintf port "        { uint8_t r = ~a ^ ~a; ~a = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }~%"
              (reg->field (cadr op)) (reg->field (caddr op)) (reg->field (cadr op))))

    ((cmp)
     (fprintf port "        { uint8_t a = ~a, b = ~a, r = (a - b) & 0xFF; cpu->P = (cpu->P & 0x7C) | (a >= b ? 1 : 0) | (a == b ? 2 : 0) | (r & 0x80); }~%"
              (reg->field (cadr op)) (reg->field (caddr op))))

    ((bit)
     (fprintf port "        { uint8_t a = ~a, b = ~a, r = a & b; cpu->P = (cpu->P & 0x3D) | (r == 0 ? 2 : 0) | (b & 0xC0); }~%"
              (reg->field (cadr op)) (reg->field (caddr op))))

    ((asl)
     (fprintf port "        { uint8_t o = ~a, n = (o << 1) & 0xFF; ~a = n; cpu->P = (cpu->P & 0x7C) | ((o >> 7) & 1) | (n == 0 ? 2 : 0) | (n & 0x80); }~%"
              (reg->field (cadr op)) (reg->field (cadr op))))

    ((lsr)
     (fprintf port "        { uint8_t o = ~a, n = o >> 1; ~a = n; cpu->P = (cpu->P & 0x7C) | (o & 1) | (n == 0 ? 2 : 0); }~%"
              (reg->field (cadr op)) (reg->field (cadr op))))

    ((rol)
     (fprintf port "        { uint8_t o = ~a, c = cpu->P & 1, n = ((o << 1) | c) & 0xFF; ~a = n; cpu->P = (cpu->P & 0x7C) | ((o >> 7) & 1) | (n == 0 ? 2 : 0) | (n & 0x80); }~%"
              (reg->field (cadr op)) (reg->field (cadr op))))

    ((ror)
     (fprintf port "        { uint8_t o = ~a, c = cpu->P & 1, n = (o >> 1) | (c << 7); ~a = n; cpu->P = (cpu->P & 0x7C) | (o & 1) | (n == 0 ? 2 : 0) | (n & 0x80); }~%"
              (reg->field (cadr op)) (reg->field (cadr op))))

    ((inc)
     (fprintf port "        { uint8_t n = (~a + 1) & 0xFF; ~a = n; cpu->P = (cpu->P & 0x7D) | (n == 0 ? 2 : 0) | (n & 0x80); }~%"
              (reg->field (cadr op)) (reg->field (cadr op))))

    ;; Increment without setting flags - for internal address calculations
    ((inc-nf)
     (fprintf port "        ~a = (~a + 1) & 0xFF;~%"
              (reg->field (cadr op)) (reg->field (cadr op))))

    ((dec)
     (fprintf port "        { uint8_t n = (~a - 1) & 0xFF; ~a = n; cpu->P = (cpu->P & 0x7D) | (n == 0 ? 2 : 0) | (n & 0x80); }~%"
              (reg->field (cadr op)) (reg->field (cadr op))))

    ((pc+1)
     (fprintf port "        cpu->PC = (cpu->PC + 1) & 0xFFFF;~%"))

    ((sp+1)
     (fprintf port "        cpu->SP = (cpu->SP + 1) & 0xFF;~%"))

    ((sp-1)
     (fprintf port "        cpu->SP = (cpu->SP - 1) & 0xFF;~%"))

    ((set-adh-zero)
     (fprintf port "        cpu->ADH = 0;~%"))

    ((branch-rel)
     (fprintf port "        if (") (emit-cond port (cadr op)) (fprintf port ") {~%")
     (fprintf port "            cpu->branch_taken = 1; uint16_t old = cpu->PC;~%")
     (fprintf port "            cpu->PC = (cpu->PC + (int8_t)~a) & 0xFFFF;~%" (reg->field (caddr op)))
     (fprintf port "            cpu->page_cross = ((old & 0xFF00) != (cpu->PC & 0xFF00));~%")
     (fprintf port "        } else { cpu->branch_taken = 0; cpu->page_cross = 0; }~%"))

    ;; branch-decide: like branch-rel but does NOT update PC.
    ;; Used by the dot-accurate branch sequence so the dummy reads in the
    ;; following cycles can hit the correct addresses (uncorrected PC for
    ;; cycle 3, intermediate PC for cycle 4 on page cross).
    ((branch-decide)
     (fprintf port "        if (") (emit-cond port (cadr op)) (fprintf port ") {~%")
     (fprintf port "            cpu->branch_taken = 1;~%")
     (fprintf port "            uint16_t _np = (cpu->PC + (int8_t)~a) & 0xFFFF;~%" (reg->field (caddr op)))
     (fprintf port "            cpu->page_cross = ((cpu->PC & 0xFF00) != (_np & 0xFF00));~%")
     (fprintf port "        } else { cpu->branch_taken = 0; cpu->page_cross = 0; }~%"))

    ;; branch-update-pcl: add offset to PCL only (PCH unchanged).
    ;; If page_cross is set, PCH will be wrong until branch-correct-pch.
    ((branch-update-pcl)
     (fprintf port "        cpu->PC = (cpu->PC & 0xFF00) | ((cpu->PC + (int8_t)~a) & 0xFF);~%"
              (reg->field (cadr op))))

    ;; branch-correct-pch: adjust PCH for page-crossing branch.
    ;; Negative offset → PCH -= 1; positive offset → PCH += 1.
    ((branch-correct-pch)
     (fprintf port "        if ((int8_t)~a < 0) cpu->PC = (cpu->PC - 0x100) & 0xFFFF;~%"
              (reg->field (cadr op)))
     (fprintf port "        else cpu->PC = (cpu->PC + 0x100) & 0xFFFF;~%"))

    ((select-interrupt-vector)
     (fprintf port "        cpu->interrupt_vector = cpu->nmi_pending ? 0xFFFA : 0xFFFE;~%")
     (fprintf port "        cpu->nmi_pending = 0;~%"))

    ((prep-push-p)
     (case (cadr op)
       ((php) (fprintf port "        cpu->DL = cpu->P | 0x30;~%"))
       ((brk) (fprintf port "        cpu->DL = cpu->P | 0x30;~%"))
       (else (fprintf port "        cpu->DL = (cpu->P | 0x20) & ~~0x10;~%"))))

    ;; Unofficial opcode operations
    ((store-ax)
     (fprintf port "        cpu->mem_write(cpu, ~a, cpu->A & cpu->X);~%" (addr->expr (cadr op))))

    ((set-c-from-n)
     (fprintf port "        cpu->P = (cpu->P & 0xFE) | ((cpu->A >> 7) & 1);~%"))

    ((arr)
     ;; ARR: AND with immediate, then ROR, with special flag handling
     ;; C = bit 6, V = bit 6 XOR bit 5
     (fprintf port "        { uint8_t t = ~a & ~a; uint8_t r = (t >> 1) | ((cpu->P & 1) << 7);~%"
              (reg->field (cadr op)) (reg->field (caddr op)))
     (fprintf port "          ~a = r; cpu->P = (cpu->P & 0x3C) | ((r >> 6) & 1) | (r == 0 ? 2 : 0) | (r & 0x80) | (((r >> 6) ^ (r >> 5)) & 1 ? 0x40 : 0); }~%"
              (reg->field (cadr op))))

    ((axs)
     ;; AXS: X = (A & X) - imm, sets NZC like CMP
     (fprintf port "        { uint8_t t = cpu->A & cpu->X; int16_t r = t - ~a; ~a = r & 0xFF;~%"
              (reg->field (caddr op)) (reg->field (cadr op)))
     (fprintf port "          cpu->P = (cpu->P & 0x7C) | (r >= 0 ? 1 : 0) | ((r & 0xFF) == 0 ? 2 : 0) | (r & 0x80); }~%"))

    ((las-op)
     ;; LAS: A = X = SP = memory & SP
     (fprintf port "        { uint8_t v = cpu->DL & cpu->SP; cpu->A = cpu->X = cpu->SP = v;~%")
     (fprintf port "          cpu->P = (cpu->P & 0x7D) | (v == 0 ? 2 : 0) | (v & 0x80); }~%"))

    ((tas-op)
     ;; TAS: SP = A & X, store A & X & (high + 1)
     (fprintf port "        cpu->SP = cpu->A & cpu->X; cpu->DL = cpu->SP & ((~a + 1) & 0xFF);~%"
              (reg->field (cadr op))))

    ((sha-op)
     ;; SHA: store A & X & (high + 1)
     (fprintf port "        cpu->DL = cpu->A & cpu->X & ((~a + 1) & 0xFF);~%"
              (reg->field (cadr op))))

    ((shx-op)
     ;; SHX: store X & (high + 1)
     (fprintf port "        cpu->DL = cpu->X & ((~a + 1) & 0xFF);~%"
              (reg->field (cadr op))))

    ((shy-op)
     ;; SHY: store Y & (high + 1)
     (fprintf port "        cpu->DL = cpu->Y & ((~a + 1) & 0xFF);~%"
              (reg->field (cadr op))))))

(define (bus-op? form)
  (and (pair? form) (memq (car form) '(fetch read write dummy))))

;; Returns the C expression for the address of a bus op.
;; Uses addr->expr (side-effect-free) for the peek function.
;; - (fetch dl)        → cpu->PC
;; - (fetch dl pc)     → cpu->PC
;; - (read dl ad)      → addr->expr of "ad" (third element)
;; - (dummy ad)        → addr->expr of "ad" (second element)
;; - (write ad reg)    → #f (DMC DMA cannot halt on write cycles)
(define (bus-op-addr-expr op)
  (case (car op)
    ((fetch) "cpu->PC")
    ((read)  (addr->expr (caddr op)))
    ((dummy) (addr->expr (cadr op)))
    ((write) #f)  ;; DMC DMA can't interrupt a write cycle
    (else #f)))

;; Returns the bus op (or #f) that DMA can halt on for this cycle.
;; Walks the cycle's forms looking for a read/dummy/fetch op (not a write).
(define (cycle-read-bus-op cycle-forms)
  (find (lambda (f)
          (and (pair? f)
               (memq (car f) '(fetch read dummy))))
        cycle-forms))

;; Find the first read-style bus op in a cycle body (fetch/read/dummy) and
;; return its address expression. Returns #f if the cycle is a pure write
;; cycle (DMC DMA cannot halt on those) or has no bus access at all.
(define (cycle-bus-addr cycle-forms)
  (let ((bus-op (cycle-read-bus-op cycle-forms)))
    (and bus-op (bus-op-addr-expr bus-op))))

(define (record-case-addr! upc addr-expr)
  (set! *peek-addrs* (cons (cons upc addr-expr) *peek-addrs*)))

(define (record-write-case! upc)
  (set! *write-cases* (cons upc *write-cases*)))

;; Returns the bus op category for a cycle:
;;   'read    — fetch/read/dummy (DMC DMA can halt)
;;   'write   — write only (DMC DMA cannot halt — must wait)
;;   'none    — no bus access (rare; treat as 'read for safety)
(define (cycle-bus-category cycle-forms)
  (let* ((has-read  (any (lambda (f)
                           (and (pair? f)
                                (memq (car f) '(fetch read dummy))))
                         cycle-forms))
         (has-write (any (lambda (f)
                           (and (pair? f) (eq? (car f) 'write)))
                         cycle-forms)))
    (cond (has-read 'read)
          (has-write 'write)
          (else 'none))))

(define (emit-cycle-forms port forms)
  (for-each
   (lambda (form)
     (when (pair? form)
       (cond
        ((bus-op? form) (emit-bus-op port form))
        ((not (eq? (car form) 'assert)) (emit-internal-op port form)))))
   forms))

;; ============================================================================
;; State emission - fixed to handle when blocks properly
;; ============================================================================

;; Compute the final skip target (skips past ALL consecutive when blocks)
(define (compute-final-skip-target remaining upc has-dispatch final-target)
  (let loop ((items remaining) (cur-upc upc))
    (let ((next-item (find (lambda (x) (and (pair? x) (memq (car x) '(cycle when)))) items)))
      (cond
       ;; No more items - go to dispatch or final
       ((not next-item)
        (cond (has-dispatch 'dispatch)
              (else final-target)))
       ;; Next is a when block - skip it and continue
       ((eq? (car next-item) 'when)
        (let ((when-cycles (count-cycles (cddr next-item))))
          (loop (cdr (member next-item items)) (+ cur-upc when-cycles))))
       ;; Next is a regular cycle - that's our target
       (else cur-upc)))))

;; Emit the uPC jump at the end of a cycle
;; If the next item is a when block, emit a conditional jump
(define (emit-upc-jump port next-item remaining next-upc has-dispatch final-target)
  (cond
   ;; Next is a when block - emit conditional jump
   ((and next-item (pair? next-item) (eq? (car next-item) 'when))
    (let* ((cond-expr (cadr next-item))
           (when-body (cddr next-item))
           (when-cycles (count-cycles when-body))
           (skip-upc (+ next-upc when-cycles))
           (after-when (cdr remaining))
           ;; Skip past ALL consecutive when blocks when condition is false
           (skip-target (compute-final-skip-target after-when skip-upc has-dispatch final-target)))
      ;; If condition true, go to the when block; if false, skip all when blocks
      (fprintf port "        if (") (emit-cond port cond-expr) (fprintf port ") { cpu->uPC = ~a; return; }~%" next-upc)
      (cond
       ((eq? skip-target 'dispatch)
        (fprintf port "        cpu->uPC = cpu_entry[cpu->IR]; return;~%"))
       (else
        (fprintf port "        cpu->uPC = ~a; return;~%" skip-target)))))
   ;; Next is a regular cycle
   (next-item
    (fprintf port "        cpu->uPC = ~a; return;~%" next-upc))
   ;; No more items
   (has-dispatch
    (fprintf port "        cpu->uPC = cpu_entry[cpu->IR]; return;~%"))
   (else
    (fprintf port "        cpu->uPC = ~a; return;~%" final-target))))

(define (emit-state port state-name body)
  (let* ((expanded (expand-templates body))
         (base-upc (assoc-ref *state-base* state-name))
         ;; Default to fetch state - implicit (goto fetch) at end of every instruction
         (final-target (or (assoc-ref *state-base* 'fetch) 0))
         (has-dispatch #f)
         (has-poll-interrupts #f))

    ;; Find explicit goto/dispatch/poll-interrupts (overrides default)
    (for-each
     (lambda (item)
       (when (pair? item)
         (case (car item)
           ((goto) (set! final-target (or (assoc-ref *state-base* (cadr item)) 0)))
           ((dispatch) (set! has-dispatch #t))
           ((poll-interrupts) (set! has-poll-interrupts #t))
           ;; Also check inside cycles for poll-interrupts
           ((cycle) 
            (for-each
             (lambda (op)
               (when (and (pair? op) (eq? (car op) 'poll-interrupts))
                 (set! has-poll-interrupts #t)))
             (cdr item))))))
     expanded)

    ;; Build inline poll code if needed.
    ;; The IRQ poll uses cpu->effective_i, which is the I flag value with
    ;; 1-instruction lag for CLI/SEI/PLP (matching real 6502 behavior).
    (define poll-code
      (if has-poll-interrupts
          (let ((nmi-base (or (assoc-ref *state-base* 'nmi-handler) 0))
                (irq-base (or (assoc-ref *state-base* 'irq-handler) 0))
                (reset-base (or (assoc-ref *state-base* 'reset-handler) 0)))
            (lambda (port)
              (fprintf port "        if (cpu->reset_pending) { cpu->reset_pending = 0; cpu->uPC = ~a; return; }~%" reset-base)
              (fprintf port "        if (cpu->nmi_armed) { cpu->nmi_armed = 0; cpu->nmi_pending = 0; cpu->uPC = ~a; return; }~%" nmi-base)
              ;; IRQ recognition is delayed by 1 instruction. The "armed" flag
              ;; is set during the previous instruction's case-0 poll if the
              ;; IRQ was pending then; this instruction's case 0 fires the IRQ
              ;; if armed is set and effective_i is 0. Then re-arm based on
              ;; current irq_pending. Mirrors real hardware "poll happens
              ;; before the last cycle of an instruction, IRQ runs at the
              ;; next opcode fetch" two-stage pipeline.
              (fprintf port "        if (cpu->irq_armed) { cpu->irq_armed = 0; cpu->irq_pending = 0; cpu->uPC = ~a; return; }~%" irq-base)

              (fprintf port "        cpu->effective_i = (cpu->P >> 2) & 1;~%")))
          #f))

    ;; Emit cycles (poll is embedded in first cycle, not separate)
    (let loop ((items expanded) (upc base-upc) (first-cycle #t))
      (unless (null? items)
        (let ((item (car items))
              (remaining (cdr items)))
          (cond
           ;; Regular cycle
           ((and (pair? item) (eq? (car item) 'cycle))
            (let ((addr (cycle-bus-addr (cdr item))))
              (when addr (record-case-addr! upc addr)))
            (when (eq? (cycle-bus-category (cdr item)) 'write)
              (record-write-case! upc))
            ;; If this cycle contains an SHA/SHX/SHY/TAS page-fixup op, the
            ;; DMC DMA halting on this uPC sets cpu->ignore_h before the
            ;; cpu_step runs, which alters the H register the SHA-family
            ;; instruction uses. Track the uPC so the DMA halt code can
            ;; recognise it.
            (when (any (lambda (f)
                         (and (pair? f)
                              (memq (car f) '(sha-page-fixup shx-page-fixup
                                              shy-page-fixup tas-page-fixup))))
                       (cdr item))
              (set! *sha-fixup-cases* (cons upc *sha-fixup-cases*)))
            (fprintf port "    case ~a: /* ~a */~%" upc state-name)
            ;; Embed interrupt poll in first cycle if needed
            (when (and first-cycle poll-code)
              (poll-code port))
            ;; Check if this cycle is followed by (when page-cross ...).
            ;; If so, we must emit bus ops (reads) BEFORE the page-cross check,
            ;; and non-bus ops (the ALU operation) AFTER it.  This prevents the
            ;; operation from executing twice with wrong data on page crossing.
            (let* ((next-upc (+ upc 1))
                   (next-item (find (lambda (x) (and (pair? x) (memq (car x) '(cycle when)))) remaining))
                   (followed-by-page-cross
                    (and next-item (pair? next-item)
                         (eq? (car next-item) 'when)
                         (eq? (cadr next-item) 'page-cross))))
              (if followed-by-page-cross
                  ;; Split: bus ops → page-cross bail → ALU ops → done return
                  (let* ((forms (cdr item))
                         (bus-forms (filter bus-op? forms))
                         (non-bus-forms (filter (lambda (f) (and (pair? f) (not (bus-op? f))
                                                                (not (eq? (car f) 'assert)))) forms))
                         ;; Compute where to jump on page-cross (into the when block)
                         (when-body (cddr next-item))
                         (when-cycles (count-cycles when-body))
                         (skip-upc (+ next-upc when-cycles))
                         (after-when (cdr remaining))
                         (skip-target (compute-final-skip-target after-when skip-upc has-dispatch final-target)))
                    ;; Emit bus reads
                    (emit-cycle-forms port bus-forms)
                    ;; Emit page-cross conditional bail (only the conditional part)
                    (fprintf port "        if (") (emit-cond port 'page-cross) (fprintf port ") { cpu->uPC = ~a; return; }~%" next-upc)
                    ;; Emit non-bus ops (ALU operation) — only reached when NO page cross
                    (emit-cycle-forms port non-bus-forms)
                    ;; Emit the "done" jump for the no-page-cross path
                    (cond
                     ((eq? skip-target 'dispatch)
                      (fprintf port "        cpu->uPC = cpu_entry[cpu->IR]; return;~%"))
                     (else
                      (fprintf port "        cpu->uPC = ~a; return;~%" skip-target))))
                  ;; Normal: emit all ops, then jump
                  (begin
                    (emit-cycle-forms port (cdr item))
                    (emit-upc-jump port next-item remaining next-upc has-dispatch final-target)))
              (loop remaining next-upc #f)))

            ;; When block - emit its operations (previous cycle handles the conditional jump)
            ((and (pair? item) (eq? (car item) 'when))
             (let* ((cond-expr (cadr item))
                    (when-body (cddr item))
                    (when-cycles (count-cycles when-body))
                    (skip-upc (+ upc when-cycles)))

               ;; Check if when block contains only non-cycle operations (goto, dispatch, etc.)
               (let ((has-cycles (any (lambda (x) (and (pair? x) (eq? (car x) 'cycle))) when-body)))
                 (if has-cycles
                     ;; Emit cycles inside when block (original logic)
                     (let wloop ((wbody when-body) (wupc upc))
                       (unless (null? wbody)
                         (let ((witem (car wbody))
                               (wremaining (cdr wbody)))
                           (when (and (pair? witem) (eq? (car witem) 'cycle))
                             (let ((addr (cycle-bus-addr (cdr witem))))
                               (when addr (record-case-addr! wupc addr)))
                             (when (eq? (cycle-bus-category (cdr witem)) 'write)
                               (record-write-case! wupc))
                             (fprintf port "    case ~a: /* ~a when ~a */~%" wupc state-name cond-expr)
                             (emit-cycle-forms port (cdr witem))
                             (let* ((next-wupc (+ wupc 1))
                                    (next-in-when (find (lambda (x) (and (pair? x) (eq? (car x) 'cycle))) wremaining))
                                    (next-after-when (find (lambda (x) (and (pair? x) (memq (car x) '(cycle when)))) remaining)))
                               ;; If more cycles in this when block, go to them
                               (if next-in-when
                                   (fprintf port "        cpu->uPC = ~a; return;~%" next-wupc)
                                   ;; Last cycle in when - emit conditional jump to next when or final
                                   (emit-upc-jump port next-after-when remaining skip-upc has-dispatch final-target))
                               (wloop wremaining next-wupc))))))
                     ;; When block has no cycles - emit conditional jump directly
                     (begin
                       (fprintf port "    case ~a: /* ~a when ~a */~%" upc state-name cond-expr)
                       ;; Process non-cycle operations in when block
                       (let wloop ((wbody when-body))
                         (unless (null? wbody)
                           (let ((witem (car wbody))
                                 (wremaining (cdr wbody)))
                             (when (pair? witem)
                               (case (car witem)
                                 ((goto) 
                                  (let ((target-state (cadr witem))
                                        (target-upc (or (assoc-ref *state-base* target-state) 0)))
                                    (fprintf port "        cpu->uPC = ~a; return;~%" target-upc)))
                                 ((dispatch)
                                  (fprintf port "        cpu->uPC = cpu_entry[cpu->IR]; return;~%"))
                                 ((poll-interrupts)
                                  (error "poll-interrupts not supported in when blocks"))
                                 (else
                                  (error "Unsupported operation in when block without cycles" witem))))
                             (wloop wremaining)))))))

               (loop remaining skip-upc #f)))

           ;; Skip goto/dispatch/poll-interrupts (already handled)
           ((and (pair? item) (memq (car item) '(goto dispatch poll-interrupts)))
            (loop remaining upc first-cycle))

           (else (loop remaining upc first-cycle))))))))

;; ============================================================================
;; Header emission
;; ============================================================================

(define (emit-header port)
  (fprintf port "/* Auto-generated 6502 microcode - DO NOT EDIT */~%")
  (fprintf port "#include <stdint.h>~%#include <stdbool.h>~%~%")
  (fprintf port "typedef struct CPU {~%")
  (fprintf port "    uint8_t A, X, Y, SP, P, IR, DL, ADL, ADH;~%")
  (fprintf port "    uint16_t PC, uPC;~%")
  (fprintf port "    bool page_cross, branch_taken;~%")
  (fprintf port "    bool irq_pending, nmi_pending, reset_pending;~%")
  (fprintf port "    bool rdy;  /* RDY line - when false, CPU is halted (for DMA) */~%")
  (fprintf port "    uint16_t last_read_addr;  /* Last address read by CPU (for DMA halt cycles) */~%")
  (fprintf port "    uint8_t effective_i; /* I flag value used for next IRQ poll (1-instr delayed for CLI/SEI/PLP) */~%")
  (fprintf port "    uint8_t ignore_h;  /* SHA/SHX/SHY/TAS: skip H register in store value when DMA halts on the dummy-read cycle (AccuracyCoin SHA test sub-test 7+, mirrors C# Emulator.cs IgnoreH) */~%")
  (fprintf port "    uint16_t interrupt_vector;~%    uint8_t irq_sampled;~%    uint8_t nmi_sampled, nmi_armed;~%    uint8_t irq_armed; /* Interrupt sampled before the final cycle, dispatched at fetch */~%")
  (fprintf port "    uint64_t cycles;~%")
  (fprintf port "    uint8_t (*mem_read)(struct CPU *cpu, uint16_t addr);~%")
  (fprintf port "    void (*mem_write)(struct CPU *cpu, uint16_t addr, uint8_t val);~%")
  (fprintf port "    void *user_data;~%")
  (fprintf port "} CPU;~%~%")
  (fprintf port "extern const uint16_t cpu_entry[256];~%~%")
  (fprintf port "static inline void cpu_init(CPU *cpu) {~%")
  (fprintf port "    cpu->A = cpu->X = cpu->Y = 0; cpu->SP = 0xFD; cpu->P = 0x24;~%")
  (fprintf port "    cpu->PC = cpu->uPC = cpu->IR = cpu->DL = cpu->ADL = cpu->ADH = 0;~%")
  (fprintf port "    cpu->page_cross = cpu->branch_taken = 0;~%")
  (fprintf port "    cpu->irq_pending = cpu->nmi_pending = cpu->reset_pending = 0;~%")
  (fprintf port "    cpu->rdy = true;  /* CPU ready to run */~%")
  (fprintf port "    cpu->last_read_addr = 0;~%")
  (fprintf port "    cpu->effective_i = 1;  /* I flag set at init */~%")
  (fprintf port "    cpu->ignore_h = 0;~%")
  (fprintf port "    cpu->interrupt_vector = 0;~%")
  (fprintf port "    cpu->irq_sampled = cpu->nmi_sampled = 0;~%")
  (fprintf port "    cpu->irq_armed = cpu->nmi_armed = 0;~%")
  (fprintf port "    cpu->cycles = 0;~%")
  (fprintf port "}~%~%"))

;; ============================================================================
;; Main
;; ============================================================================

(define (compile-to-c input-port output-port header-port)
  (let loop ((forms '()))
    (let ((form (read input-port)))
      (if (eof-object? form)
          (begin
            (collect-definitions! (reverse forms))

            ;; Assign base uPC
            (let assign ((states (reverse *states*)) (upc 0))
              (unless (null? states)
                (let* ((state (car states))
                       (expanded (expand-templates (cdr state)))
                       (cycles (count-cycles expanded)))
                  (set! *state-base* (cons (cons (car state) upc) *state-base*))
                  (assign (cdr states) (+ upc cycles)))))

            ;; Emit header file (struct + declarations)
            (emit-header header-port)
            (fprintf header-port "void cpu_step(CPU *cpu);~%")
            (fprintf header-port "uint16_t cpu_get_next_read_addr(CPU *cpu);~%")
            (fprintf header-port "bool cpu_next_is_write(CPU *cpu);~%")
            (fprintf header-port "bool cpu_next_is_sha_dummy_read(CPU *cpu);~%")
            ;; .c file includes the generated header
            (fprintf output-port "/* Auto-generated 6502 microcode - DO NOT EDIT */~%")
            (fprintf output-port "#include \"cpu_gen.h\"~%~%")

            (fprintf output-port "static void cpu_microcycle(CPU *cpu) {~%")
            (fprintf output-port "    cpu->cycles++;~%")
            (fprintf output-port "    /* RDY line - when low, CPU is halted (for DMA) */~%")
            (fprintf output-port "    if (!cpu->rdy) return;~%")
            (fprintf output-port "    switch (cpu->uPC) {~%")

            (for-each
             (lambda (state) (emit-state output-port (car state) (cdr state)))
             (reverse *states*))

            (fprintf output-port "    default: cpu->uPC = 0; return;~%    }~%}~%~%")

            (fprintf output-port "void cpu_step(CPU *cpu) {~%")
            (fprintf output-port "    bool running = cpu->rdy;~%    uint16_t old_upc = cpu->uPC;~%")
            (fprintf output-port "    uint8_t poll = cpu->irq_sampled;~%    cpu->nmi_sampled = cpu->nmi_pending;~%")
            (fprintf output-port "    cpu->irq_sampled = cpu->irq_pending && !(cpu->P & 4);~%")
            (fprintf output-port "    cpu_microcycle(cpu);~%")
            (fprintf output-port "    if (running && cpu->uPC == 0) cpu->irq_armed = old_upc > ~a && cpu->IR != 0 ? poll : 0;~%" (assoc-ref *state-base* 'illegal))
            (fprintf output-port "    if (running && cpu->uPC == 0) cpu->nmi_armed = old_upc > ~a && cpu->IR != 0 ? cpu->nmi_sampled : 0;~%" (assoc-ref *state-base* 'illegal))
            (fprintf output-port "}~%~%")

            ;; Emit cpu_get_next_read_addr — returns the address the CPU
            ;; would drive on its bus during the next cpu_step call,
            ;; based on the current uPC. Used by DMA halt cycles to
            ;; re-read from the same address the CPU was about to use.
            (fprintf output-port "uint16_t cpu_get_next_read_addr(CPU *cpu) {~%")
            (fprintf output-port "    switch (cpu->uPC) {~%")
            (for-each
             (lambda (pair)
               (fprintf output-port "    case ~a: return ~a;~%" (car pair) (cdr pair)))
             (reverse *peek-addrs*))
            (fprintf output-port "    default: return cpu->last_read_addr;~%")
            (fprintf output-port "    }~%}~%~%")

            ;; Emit cpu_next_is_write — returns true if the next cpu_step
            ;; cycle is a pure write cycle. DMC DMA cannot halt on write
            ;; cycles (per real hardware), so the caller should defer.
            (fprintf output-port "bool cpu_next_is_write(CPU *cpu) {~%")
            (fprintf output-port "    switch (cpu->uPC) {~%")
            (for-each
             (lambda (upc)
               (fprintf output-port "    case ~a: return true;~%" upc))
             (reverse *write-cases*))
            (fprintf output-port "    default: return false;~%")
            (fprintf output-port "    }~%}~%~%")

            ;; Emit cpu_next_is_sha_dummy_read — returns true if the next
            ;; cpu_step cycle is a SHA/SHX/SHY/TAS dummy-read pre-write
            ;; cycle. DMC DMA halting here makes the SHA-family instruction
            ;; ignore the H register in the value it stores (AccuracyCoin
            ;; SHA test sub-test 7+, C# Emulator.cs IgnoreH).
            (fprintf output-port "bool cpu_next_is_sha_dummy_read(CPU *cpu) {~%")
            (fprintf output-port "    switch (cpu->uPC) {~%")
            (for-each
             (lambda (upc)
               (fprintf output-port "    case ~a: return true;~%" upc))
             (reverse *sha-fixup-cases*))
            (fprintf output-port "    default: return false;~%")
            (fprintf output-port "    }~%}~%~%")

            ;; Entry table
            (let ((illegal-base (or (assoc-ref *state-base* 'illegal) 0)))
              (fprintf output-port "const uint16_t cpu_entry[256] = {~%")
              (do ((i 0 (+ i 1))) ((= i 256))
                (when (= (modulo i 16) 0) (display "    " output-port))
                (display (or (and (vector-ref *opcodes* i)
                                  (assoc-ref *state-base* (vector-ref *opcodes* i)))
                             illegal-base)
                         output-port)
                (when (< i 255) (display "," output-port))
                (when (= (modulo i 16) 15) (newline output-port)))
              (fprintf output-port "};~%")))
          (loop (cons form forms))))))

;; Entry point
(let ((args (command-line-arguments)))
  (if (< (length args) 3)
      (begin
        (display "Usage: csi -s dsl2c.scm <input.dsl> <output.c> <output.h>\n" (current-error-port))
        (exit 1))
      (call-with-input-file (car args)
        (lambda (in)
          (call-with-output-file (cadr args)
            (lambda (out)
              (call-with-output-file (caddr args)
                (lambda (hdr)
                  (fprintf hdr "#ifndef CPU_GEN_H~%#define CPU_GEN_H~%~%")
                  (compile-to-c in out hdr)
                  (fprintf hdr "~%#endif /* CPU_GEN_H */~%")))))))))
