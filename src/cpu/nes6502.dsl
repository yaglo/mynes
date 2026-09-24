;; =============================================================================
;; NES 6502 (2A03) Complete Microcode Definition
;; Includes all official and unofficial opcodes
;; =============================================================================

;; =============================================================================
;; CYCLE MACROS - Reusable cycle patterns
;; =============================================================================

;; Basic read/write operations
(defcycle read-to-dl         (read dl ad))
(defcycle write-from-dl      (write ad dl))
(defcycle write-from-reg     (write ad reg))  ;; Note: 'reg' must be substituted by template
(defcycle dummy-read-ad      (dummy ad))
(defcycle dummy-read-pc      (dummy pc))
(defcycle dummy-read-sp      (dummy sp))

;; Fetch operations
(defcycle fetch-adl          (fetch adl pc))
(defcycle fetch-adh          (fetch adh pc))
(defcycle fetch-to-dl        (fetch dl pc))

;; Zero-page setup (fetch pointer + set high byte to 0)
(defcycle fetch-zp-addr      (fetch adl pc) (set-adh-zero))
(defcycle fetch-zp-ptr       (fetch dl pc) (set-adh-zero))

;; Indexed addressing - fetch high byte and add index
(defcycle fetch-adh-add-x    (fetch adh pc) (add8-latch-carry adl adl x))
(defcycle fetch-adh-add-y    (fetch adh pc) (add8-latch-carry adl adl y))

;; Page crossing fixup (re-read with corrected high byte)
(defcycle fixup-page-read    (read dl ad) (adc8-from-pagecross adh adh))

;; SHA page crossing fixup - corrupts address to (ADH+1) & A & X
(defcycle sha-fixup-page     (read dl ad) (sha-page-fixup))

;; SHX page crossing fixup - corrupts address to (ADH+1) & X
(defcycle shx-fixup-page     (read dl ad) (shx-page-fixup))

;; SHY page crossing fixup - corrupts address to (ADH+1) & Y
(defcycle shy-fixup-page     (read dl ad) (shy-page-fixup))

;; TAS page crossing fixup - SP = A & X, DL = SP & (ADH+1), on page cross corrupt ADH
(defcycle tas-fixup-page     (read dl ad) (tas-page-fixup))

;; Indirect addressing helpers - use inc-nf to NOT corrupt flags
(defcycle izx-read-ptr-high  (inc-nf adl) (read adh ad))
(defcycle izy-fetch-ptr-high (inc-nf adl) (read adh ad) (add8-latch-carry adl dl y))
(defcycle setup-ptr-dummy    (mov adl dl) (dummy ad))
(defcycle read-eff-low       (mov adl dl) (read dl ad))
(defcycle add-x-read-low     (add8-latch-carry adl adl x) (read dl ad))

;; Indexed zero-page helpers
(defcycle dummy-add-x        (dummy ad) (add8-latch-carry adl adl x))
(defcycle dummy-add-y        (dummy ad) (add8-latch-carry adl adl y))

;; RMW helpers - final write with flag set
(defcycle write-nz-dl        (write ad dl) (nz dl))

;; =============================================================================
;; CORE STATES
;; =============================================================================

(state fetch
  (cycle
    (poll-interrupts)
    (fetch ir pc))
  (dispatch))

;; =============================================================================
;; INTERRUPT HANDLERS
;; =============================================================================

;; NMI Handler: 7 cycles (like BRK but reads NMI vector, B flag clear)
;; Cycle 1 is the "dispatch" cycle in case 0 (no memory access)
;; Cycles 2-7 are the actual handler
(state nmi-handler
  (cycle (dummy pc))
  (cycle (write sp pch) (sp-1))
  (cycle (write sp pcl) (sp-1))
  (cycle (prep-push-p nmi) (write sp dl) (sp-1))
  (cycle (set-flag i) (snapshot-i) (read adl vec-nmi-lo))
  (cycle (read adh vec-nmi-hi) (mov pcl adl) (mov pch adh))
  (goto fetch))

;; IRQ Handler: 7 cycles (like BRK but reads IRQ vector, B flag clear)
;; Uses hijack vectors to support NMI hijacking during IRQ
(state irq-handler
  (cycle (dummy pc))
  (cycle (write sp pch) (sp-1))
  (cycle (write sp pcl) (sp-1))
  (cycle (select-interrupt-vector) (prep-push-p irq) (write sp dl) (sp-1))
  (cycle (set-flag i) (snapshot-i) (read adl vec-irq-hijack-lo))
  (cycle (read adh vec-irq-hijack-hi) (mov pcl adl) (mov pch adh))
  (goto fetch))

;; RESET Handler: Sets up CPU and reads reset vector
(state reset-handler
  (cycle (dummy pc))
  (cycle (dummy sp) (sp-1))
  (cycle (dummy sp) (sp-1))
  (cycle (dummy sp) (sp-1) (set-flag i) (snapshot-i))
  (cycle (read adl vec-reset-lo))
  (cycle (read adh vec-reset-hi))
  (cycle (mov pcl adl) (mov pch adh)))

(state illegal
  (cycle (dummy pc))
 )

;; =============================================================================
;; ADDRESSING MODE TEMPLATES
;; =============================================================================

;; Immediate: 2 cycles
(template read-imm (op reg)
  (cycle (fetch dl pc) (op reg dl))
 )

;; Zero Page: 3 cycles
(template read-zp (op reg)
  (fetch-zp-addr)
  (cycle (read dl ad) (op reg dl))
 )

;; Zero Page,X: 4 cycles
(template read-zpx (op reg)
  (fetch-zp-addr)
  (dummy-add-x)
  (cycle (read dl ad) (op reg dl))
 )

;; Zero Page,Y: 4 cycles
(template read-zpy (op reg)
  (fetch-zp-addr)
  (dummy-add-y)
  (cycle (read dl ad) (op reg dl))
 )

;; Absolute: 4 cycles
(template read-abs (op reg)
  (fetch-adl)
  (fetch-adh)
  (cycle (read dl ad) (op reg dl))
 )

;; Absolute,X: 4+ cycles (page cross adds 1)
(template read-abx (op reg)
  (fetch-adl)
  (fetch-adh-add-x)
  (cycle (read dl ad) (op reg dl))
  (when page-cross
    (cycle (adc8-from-pagecross adh adh) (read dl ad) (op reg dl)))
 )

;; Absolute,Y: 4+ cycles (page cross adds 1)
(template read-aby (op reg)
  (fetch-adl)
  (fetch-adh-add-y)
  (cycle (read dl ad) (op reg dl))
  (when page-cross
    (cycle (adc8-from-pagecross adh adh) (read dl ad) (op reg dl)))
 )

;; Indexed Indirect (zp,X): 6 cycles
(template read-izx (op reg)
  (fetch-zp-ptr)
  (setup-ptr-dummy)
  (add-x-read-low)
  (izx-read-ptr-high)
  (cycle (mov adl dl) (read dl ad) (op reg dl))
 )

;; Indirect Indexed (zp),Y: 5+ cycles (page cross adds 1)
(template read-izy (op reg)
  (fetch-zp-ptr)
  (read-eff-low)
  (izy-fetch-ptr-high)
  (cycle (read dl ad) (op reg dl))
  (when page-cross
    (cycle (adc8-from-pagecross adh adh) (read dl ad) (op reg dl)))
 )

;; =============================================================================
;; STORE TEMPLATES
;; =============================================================================

;; Zero Page Store: 3 cycles
(template store-zp (reg)
  (fetch-zp-addr)
  (cycle (write ad reg))
 )

;; Zero Page,X Store: 4 cycles
(template store-zpx (reg)
  (fetch-zp-addr)
  (dummy-add-x)
  (cycle (write ad reg))
 )

;; Zero Page,Y Store: 4 cycles
(template store-zpy (reg)
  (fetch-zp-addr)
  (dummy-add-y)
  (cycle (write ad reg))
 )

;; Absolute Store: 4 cycles
(template store-abs (reg)
  (fetch-adl)
  (fetch-adh)
  (cycle (write ad reg))
 )

;; Absolute,X Store: 5 cycles (always takes penalty cycle)
(template store-abx (reg)
  (fetch-adl)
  (fetch-adh-add-x)
  (fixup-page-read)
  (cycle (write ad reg))
 )

;; Absolute,Y Store: 5 cycles (always takes penalty cycle)
(template store-aby (reg)
  (fetch-adl)
  (fetch-adh-add-y)
  (fixup-page-read)
  (cycle (write ad reg))
 )

;; Indexed Indirect (zp,X) Store: 6 cycles
(template store-izx (reg)
  (fetch-zp-ptr)
  (setup-ptr-dummy)
  (add-x-read-low)
  (izx-read-ptr-high)
  (cycle (mov adl dl) (write ad reg))
 )

;; Indirect Indexed (zp),Y Store: 6 cycles (always takes penalty cycle)
(template store-izy (reg)
  (fetch-zp-ptr)
  (read-eff-low)
  (izy-fetch-ptr-high)
  (fixup-page-read)
  (cycle (write ad reg))
 )

;; =============================================================================
;; READ-MODIFY-WRITE TEMPLATES
;; =============================================================================

;; Zero Page RMW: 5 cycles (flags set during final write)
(template rmw-zp (op)
  (fetch-zp-addr)
  (read-to-dl)
  (cycle (write ad dl) (op dl))
  (write-nz-dl)
 )

;; Zero Page,X RMW: 6 cycles (flags set during final write)
(template rmw-zpx (op)
  (fetch-zp-addr)
  (dummy-add-x)
  (read-to-dl)
  (cycle (write ad dl) (op dl))
  (write-nz-dl)
 )

;; Absolute RMW: 6 cycles (flags set during final write)
(template rmw-abs (op)
  (fetch-adl)
  (fetch-adh)
  (read-to-dl)
  (cycle (write ad dl) (op dl))
  (write-nz-dl)
 )

;; Absolute,X RMW: 7 cycles (flags set during final write)
(template rmw-abx (op)
  (fetch-adl)
  (fetch-adh-add-x)
  (fixup-page-read)
  (read-to-dl)
  (cycle (write ad dl) (op dl))
  (write-nz-dl)
 )

;; Absolute,Y RMW: 7 cycles (flags set during final write)
(template rmw-aby (op)
  (fetch-adl)
  (fetch-adh-add-y)
  (fixup-page-read)
  (read-to-dl)
  (cycle (write ad dl) (op dl))
  (write-nz-dl)
 )

;; Indexed Indirect (zp,X) RMW: 8 cycles
(template rmw-izx (op)
  (fetch-zp-ptr)
  (setup-ptr-dummy)
  (add-x-read-low)
  (izx-read-ptr-high)
  (read-eff-low)
  (cycle (write ad dl) (op dl))
  (write-nz-dl)
 )

;; Indirect Indexed (zp),Y RMW: 8 cycles
(template rmw-izy (op)
  (fetch-zp-ptr)
  (read-eff-low)
  (izy-fetch-ptr-high)
  (fixup-page-read)
  (read-to-dl)
  (cycle (write ad dl) (op dl))
  (write-nz-dl)
 )

;; =============================================================================
;; INTERNAL OPERATION TEMPLATES
;; =============================================================================

;; Load with NZ flags
(template load-nz (dst src)
  (mov dst src)
  (nz dst))

;; ADC operation
(template do-adc (reg mem)
  (adc reg mem))

;; SBC operation
(template do-sbc (reg mem)
  (sbc reg mem))

;; AND operation
(template do-and (reg mem)
  (and reg mem))

;; ORA operation
(template do-ora (reg mem)
  (ora reg mem))

;; EOR operation
(template do-eor (reg mem)
  (eor reg mem))

;; CMP operation
(template do-cmp (reg mem)
  (cmp reg mem))

;; BIT operation
(template do-bit (reg mem)
  (bit reg mem))

;; =============================================================================
;; LOAD INSTRUCTIONS
;; =============================================================================

;; LDA - Load Accumulator
(state lda-imm (read-imm load-nz a))
(state lda-zp (read-zp load-nz a))
(state lda-zpx (read-zpx load-nz a))
(state lda-abs (read-abs load-nz a))
(state lda-abx (read-abx load-nz a))
(state lda-aby (read-aby load-nz a))
(state lda-izx (read-izx load-nz a))
(state lda-izy (read-izy load-nz a))

;; LDX - Load X Register
(state ldx-imm (read-imm load-nz x))
(state ldx-zp (read-zp load-nz x))
(state ldx-zpy (read-zpy load-nz x))
(state ldx-abs (read-abs load-nz x))
(state ldx-aby (read-aby load-nz x))

;; LDY - Load Y Register
(state ldy-imm (read-imm load-nz y))
(state ldy-zp (read-zp load-nz y))
(state ldy-zpx (read-zpx load-nz y))
(state ldy-abs (read-abs load-nz y))
(state ldy-abx (read-abx load-nz y))

;; =============================================================================
;; STORE INSTRUCTIONS
;; =============================================================================

;; STA - Store Accumulator
(state sta-zp (store-zp a))
(state sta-zpx (store-zpx a))
(state sta-abs (store-abs a))
(state sta-abx (store-abx a))
(state sta-aby (store-aby a))
(state sta-izx (store-izx a))
(state sta-izy (store-izy a))

;; STX - Store X Register
(state stx-zp (store-zp x))
(state stx-zpy (store-zpy x))
(state stx-abs (store-abs x))

;; STY - Store Y Register
(state sty-zp (store-zp y))
(state sty-zpx (store-zpx y))
(state sty-abs (store-abs y))

;; =============================================================================
;; TRANSFER INSTRUCTIONS
;; =============================================================================

;; Transfer instructions (all 2 cycles)
(state tax (cycle (dummy pc) (mov x a) (nz x)))
(state tay (cycle (dummy pc) (mov y a) (nz y)))
(state txa (cycle (dummy pc) (mov a x) (nz a)))
(state tya (cycle (dummy pc) (mov a y) (nz a)))
(state tsx (cycle (dummy pc) (mov x sp) (nz x)))
(state txs (cycle (dummy pc) (mov sp x)))

;; =============================================================================
;; STACK INSTRUCTIONS
;; =============================================================================

(state pha
  (dummy-read-pc)
  (cycle (write sp a) (sp-1))
 )

(state php
  (cycle (dummy pc) (prep-push-p php))
  (cycle (write sp dl) (sp-1))
 )

(state pla
  (dummy-read-pc)
  (cycle (dummy sp) (sp+1))
  (cycle (read a sp) (nz a))
 )

(state plp
  (cycle (dummy pc))
  (cycle (dummy sp) (sp+1))
  (cycle (snapshot-i) (read p sp))  ; Save old I before pulling new P
 )

;; =============================================================================
;; ARITHMETIC INSTRUCTIONS
;; =============================================================================

;; ADC - Add with Carry
(state adc-imm (read-imm do-adc a))
(state adc-zp (read-zp do-adc a))
(state adc-zpx (read-zpx do-adc a))
(state adc-abs (read-abs do-adc a))
(state adc-abx (read-abx do-adc a))
(state adc-aby (read-aby do-adc a))
(state adc-izx (read-izx do-adc a))
(state adc-izy (read-izy do-adc a))

;; SBC - Subtract with Carry
(state sbc-imm (read-imm do-sbc a))
(state sbc-zp (read-zp do-sbc a))
(state sbc-zpx (read-zpx do-sbc a))
(state sbc-abs (read-abs do-sbc a))
(state sbc-abx (read-abx do-sbc a))
(state sbc-aby (read-aby do-sbc a))
(state sbc-izx (read-izx do-sbc a))
(state sbc-izy (read-izy do-sbc a))

;; =============================================================================
;; LOGICAL INSTRUCTIONS
;; =============================================================================

;; AND - Logical AND
(state and-imm (read-imm do-and a))
(state and-zp (read-zp do-and a))
(state and-zpx (read-zpx do-and a))
(state and-abs (read-abs do-and a))
(state and-abx (read-abx do-and a))
(state and-aby (read-aby do-and a))
(state and-izx (read-izx do-and a))
(state and-izy (read-izy do-and a))

;; ORA - Logical OR
(state ora-imm (read-imm do-ora a))
(state ora-zp (read-zp do-ora a))
(state ora-zpx (read-zpx do-ora a))
(state ora-abs (read-abs do-ora a))
(state ora-abx (read-abx do-ora a))
(state ora-aby (read-aby do-ora a))
(state ora-izx (read-izx do-ora a))
(state ora-izy (read-izy do-ora a))

;; EOR - Exclusive OR
(state eor-imm (read-imm do-eor a))
(state eor-zp (read-zp do-eor a))
(state eor-zpx (read-zpx do-eor a))
(state eor-abs (read-abs do-eor a))
(state eor-abx (read-abx do-eor a))
(state eor-aby (read-aby do-eor a))
(state eor-izx (read-izx do-eor a))
(state eor-izy (read-izy do-eor a))

;; =============================================================================
;; COMPARE INSTRUCTIONS
;; =============================================================================

;; CMP - Compare Accumulator
(state cmp-imm (read-imm do-cmp a))
(state cmp-zp (read-zp do-cmp a))
(state cmp-zpx (read-zpx do-cmp a))
(state cmp-abs (read-abs do-cmp a))
(state cmp-abx (read-abx do-cmp a))
(state cmp-aby (read-aby do-cmp a))
(state cmp-izx (read-izx do-cmp a))
(state cmp-izy (read-izy do-cmp a))

;; CPX - Compare X Register
(state cpx-imm (read-imm do-cmp x))
(state cpx-zp (read-zp do-cmp x))
(state cpx-abs (read-abs do-cmp x))

;; CPY - Compare Y Register
(state cpy-imm (read-imm do-cmp y))
(state cpy-zp (read-zp do-cmp y))
(state cpy-abs (read-abs do-cmp y))

;; =============================================================================
;; BIT TEST
;; =============================================================================

(state bit-zp (read-zp do-bit a))
(state bit-abs (read-abs do-bit a))

;; =============================================================================
;; INCREMENT/DECREMENT INSTRUCTIONS
;; =============================================================================

;; INC - Increment Memory
(state inc-zp (rmw-zp inc))
(state inc-zpx (rmw-zpx inc))
(state inc-abs (rmw-abs inc))
(state inc-abx (rmw-abx inc))

;; DEC - Decrement Memory
(state dec-zp (rmw-zp dec))
(state dec-zpx (rmw-zpx dec))
(state dec-abs (rmw-abs dec))
(state dec-abx (rmw-abx dec))

;; Register increment/decrement (all 2 cycles)
(state inx (cycle (dummy pc) (inc x) (nz x)))
(state iny (cycle (dummy pc) (inc y) (nz y)))
(state dex (cycle (dummy pc) (dec x) (nz x)))
(state dey (cycle (dummy pc) (dec y) (nz y)))

;; =============================================================================
;; SHIFT/ROTATE INSTRUCTIONS
;; =============================================================================

;; Accumulator shifts (2 cycles)
(state asl-acc (cycle (dummy pc) (asl a)))
(state lsr-acc (cycle (dummy pc) (lsr a)))
(state rol-acc (cycle (dummy pc) (rol a)))
(state ror-acc (cycle (dummy pc) (ror a)))

;; Memory shifts - ASL
(state asl-zp (rmw-zp asl))
(state asl-zpx (rmw-zpx asl))
(state asl-abs (rmw-abs asl))
(state asl-abx (rmw-abx asl))

;; Memory shifts - LSR
(state lsr-zp (rmw-zp lsr))
(state lsr-zpx (rmw-zpx lsr))
(state lsr-abs (rmw-abs lsr))
(state lsr-abx (rmw-abx lsr))

;; Memory shifts - ROL
(state rol-zp (rmw-zp rol))
(state rol-zpx (rmw-zpx rol))
(state rol-abs (rmw-abs rol))
(state rol-abx (rmw-abx rol))

;; Memory shifts - ROR
(state ror-zp (rmw-zp ror))
(state ror-zpx (rmw-zpx ror))
(state ror-abs (rmw-abs ror))
(state ror-abx (rmw-abx ror))

;; =============================================================================
;; BRANCH INSTRUCTIONS
;; =============================================================================

;; Branch template — dot-accurate dummy reads.
;; Cycle 1: fetch operand into DL, decide whether branch is taken (PC unchanged).
;; Cycle 2 (taken): dummy-read at the un-updated PC, then update PCL only.
;; Cycle 3 (page cross): dummy-read at the intermediate PC (wrong PCH), then correct PCH.
(template branch-if (cond)
  (cycle (fetch dl pc) (branch-decide cond dl))
  (when branch-taken
    (cycle (dummy pc) (branch-update-pcl dl)))
  (when page-cross
    (cycle (dummy pc) (branch-correct-pch dl)))
 )

(state bpl (branch-if (not n)))
(state bmi (branch-if n))
(state bvc (branch-if (not v)))
(state bvs (branch-if v))
(state bcc (branch-if (not c)))
(state bcs (branch-if c))
(state bne (branch-if (not z)))
(state beq (branch-if z))

;; =============================================================================
;; JUMP INSTRUCTIONS
;; =============================================================================

;; JMP Absolute: 3 cycles (T3 fetches ADH and loads PC)
(state jmp-abs
  (fetch-adl)
  (cycle (fetch adh pc) (mov pcl adl) (mov pch adh))
 )

;; JMP Indirect: 5 cycles (with 6502 page-wrap bug)
;; T5 reads high byte and loads PC
(state jmp-ind
  (fetch-adl)
  (fetch-adh)
  (read-to-dl)
  (cycle (inc-nf adl) (read adh ad) (mov pcl dl) (mov pch adh))
 )

;; JSR: 6 cycles total
(state jsr
  (fetch-adl)
  (dummy-read-sp)
  (cycle (write sp pch) (sp-1))
  (cycle (write sp pcl) (sp-1))
  (cycle (fetch adh pc) (mov pcl adl) (mov pch adh))
 )

;; RTS: 6 cycles
(state rts
  (dummy-read-pc)
  (cycle (dummy sp) (sp+1))
  (cycle (read adl sp) (sp+1))
  (cycle (read adh sp))
  (cycle (dummy ad) (mov pcl adl) (mov pch adh) (pc+1))
 )

;; RTI: 6 cycles (T6 reads PCH and loads PC)
;; Unlike CLI/SEI/PLP, RTI does NOT delay the I flag. The pulled I takes
;; effect immediately for IRQ polling — snapshot AFTER reading P.
(state rti
  (dummy-read-pc)
  (cycle (dummy sp) (sp+1))
  (cycle (read p sp) (snapshot-i) (sp+1))  ; Sync effective_i to NEW I
  (cycle (read adl sp) (sp+1))
  (cycle (read adh sp) (mov pcl adl) (mov pch adh))
 )

;; =============================================================================
;; FLAG INSTRUCTIONS (all 2 cycles: fetch + internal)
;; =============================================================================

(state clc (cycle (dummy pc) (clear-flag c)))
(state sec (cycle (dummy pc) (set-flag c)))
;; CLI: Clear Interrupt Disable - 2 cycles total (fetch + dummy/exec)
;; The (snapshot-i) before clear-flag captures the OLD I value into effective_i.
;; The next instruction's case 0 IRQ poll will use this OLD I, giving the
;; 1-instruction delay that matches real 6502 hardware.
(state cli
  (cycle (snapshot-i) (clear-flag i) (dummy pc)))

;; SEI: Set Interrupt Disable - 2 cycles total
(state sei
  (cycle (snapshot-i) (set-flag i) (dummy pc)))
(state cld (cycle (dummy pc) (clear-flag d)))
(state sed (cycle (dummy pc) (set-flag d)))
(state clv (cycle (dummy pc) (clear-flag v)))

;; =============================================================================
;; SYSTEM INSTRUCTIONS
;; =============================================================================

;; NOP: 2 cycles
(state nop (dummy-read-pc))

;; BRK: 7 cycles (T7 reads vector high and loads PC)
;; Cycle 1 reads the signature byte at PC, then increments PC
;; Uses hijack vectors to support NMI hijacking during BRK
(state brk
  (cycle (dummy pc) (pc+1))
  (cycle (write sp pch) (sp-1))
  (cycle (write sp pcl) (sp-1))
  (cycle (select-interrupt-vector) (prep-push-p brk) (write sp dl) (sp-1))
  (cycle (set-flag i) (snapshot-i) (read adl vec-irq-hijack-lo))
  (cycle (read adh vec-irq-hijack-hi) (mov pcl adl) (mov pch adh))
 )

;; =============================================================================
;; UNOFFICIAL OPCODES
;; =============================================================================

;; ---------------------------------------------------------------------------
;; LAX - LDA + LDX (Load A and X with same value)
;; ---------------------------------------------------------------------------

(template do-lax (dst src)
  (mov a src)
  (mov x src)
  (nz a))

(state lax-zp (read-zp do-lax a))
(state lax-zpy (read-zpy do-lax a))
(state lax-abs (read-abs do-lax a))
(state lax-aby (read-aby do-lax a))
(state lax-izx (read-izx do-lax a))
(state lax-izy (read-izy do-lax a))

;; LAX Immediate is unstable, but we implement it anyway
(state lax-imm (read-imm do-lax a))

;; ---------------------------------------------------------------------------
;; SAX - Store A AND X
;; ---------------------------------------------------------------------------

;; Need special handling - stores A & X without affecting flags
(state sax-zp
  (cycle (fetch adl pc) (set-adh-zero))
  (cycle (store-ax ad))
 )

(state sax-zpy
  (fetch-zp-addr)
  (dummy-add-y)
  (cycle (store-ax ad))
 )

(state sax-abs
  (fetch-adl)
  (fetch-adh)
  (cycle (store-ax ad))
 )

(state sax-izx
  (fetch-zp-ptr)
  (setup-ptr-dummy)
  (add-x-read-low)
  (izx-read-ptr-high)
  (cycle (mov adl dl) (store-ax ad))
 )

;; ---------------------------------------------------------------------------
;; DCP - DEC + CMP (Decrement memory then compare) - RMW unofficial
;; ---------------------------------------------------------------------------

(state dcp-zp
  (fetch-zp-addr) (read-to-dl)
  (cycle (write ad dl) (dec dl)) (cycle (write ad dl) (cmp a dl)))

(state dcp-zpx
  (fetch-zp-addr) (dummy-add-x) (read-to-dl)
  (cycle (write ad dl) (dec dl)) (cycle (write ad dl) (cmp a dl)))

(state dcp-abs
  (fetch-adl) (fetch-adh) (read-to-dl)
  (cycle (write ad dl) (dec dl)) (cycle (write ad dl) (cmp a dl)))

(state dcp-abx
  (fetch-adl) (fetch-adh-add-x) (fixup-page-read) (read-to-dl)
  (cycle (write ad dl) (dec dl)) (cycle (write ad dl) (cmp a dl)))

(state dcp-aby
  (fetch-adl) (fetch-adh-add-y) (fixup-page-read) (read-to-dl)
  (cycle (write ad dl) (dec dl)) (cycle (write ad dl) (cmp a dl)))

(state dcp-izx
  (fetch-zp-ptr) (setup-ptr-dummy) (add-x-read-low) (izx-read-ptr-high) (read-eff-low)
  (cycle (write ad dl) (dec dl)) (cycle (write ad dl) (cmp a dl)))

(state dcp-izy
  (fetch-zp-ptr) (read-eff-low) (izy-fetch-ptr-high) (fixup-page-read) (read-to-dl)
  (cycle (write ad dl) (dec dl)) (cycle (write ad dl) (cmp a dl)))

;; ---------------------------------------------------------------------------
;; ISC/ISB - INC + SBC (Increment memory then subtract) - RMW unofficial
;; ---------------------------------------------------------------------------

(state isc-zp
  (fetch-zp-addr) (read-to-dl)
  (cycle (write ad dl) (inc dl)) (cycle (write ad dl) (sbc a dl)))

(state isc-zpx
  (fetch-zp-addr) (dummy-add-x) (read-to-dl)
  (cycle (write ad dl) (inc dl)) (cycle (write ad dl) (sbc a dl)))

(state isc-abs
  (fetch-adl) (fetch-adh) (read-to-dl)
  (cycle (write ad dl) (inc dl)) (cycle (write ad dl) (sbc a dl)))

(state isc-abx
  (fetch-adl) (fetch-adh-add-x) (fixup-page-read) (read-to-dl)
  (cycle (write ad dl) (inc dl)) (cycle (write ad dl) (sbc a dl)))

(state isc-aby
  (fetch-adl) (fetch-adh-add-y) (fixup-page-read) (read-to-dl)
  (cycle (write ad dl) (inc dl)) (cycle (write ad dl) (sbc a dl)))

(state isc-izx
  (fetch-zp-ptr) (setup-ptr-dummy) (add-x-read-low) (izx-read-ptr-high) (read-eff-low)
  (cycle (write ad dl) (inc dl)) (cycle (write ad dl) (sbc a dl)))

(state isc-izy
  (fetch-zp-ptr) (read-eff-low) (izy-fetch-ptr-high) (fixup-page-read) (read-to-dl)
  (cycle (write ad dl) (inc dl)) (cycle (write ad dl) (sbc a dl)))

;; ---------------------------------------------------------------------------
;; SLO - ASL + ORA (Shift left then OR with accumulator) - RMW unofficial
;; ---------------------------------------------------------------------------

(state slo-zp
  (fetch-zp-addr) (read-to-dl)
  (cycle (write ad dl) (asl dl)) (cycle (write ad dl) (ora a dl)))

(state slo-zpx
  (fetch-zp-addr) (dummy-add-x) (read-to-dl)
  (cycle (write ad dl) (asl dl)) (cycle (write ad dl) (ora a dl)))

(state slo-abs
  (fetch-adl) (fetch-adh) (read-to-dl)
  (cycle (write ad dl) (asl dl)) (cycle (write ad dl) (ora a dl)))

(state slo-abx
  (fetch-adl) (fetch-adh-add-x) (fixup-page-read) (read-to-dl)
  (cycle (write ad dl) (asl dl)) (cycle (write ad dl) (ora a dl)))

(state slo-aby
  (fetch-adl) (fetch-adh-add-y) (fixup-page-read) (read-to-dl)
  (cycle (write ad dl) (asl dl)) (cycle (write ad dl) (ora a dl)))

(state slo-izx
  (fetch-zp-ptr) (setup-ptr-dummy) (add-x-read-low) (izx-read-ptr-high) (read-eff-low)
  (cycle (write ad dl) (asl dl)) (cycle (write ad dl) (ora a dl)))

(state slo-izy
  (fetch-zp-ptr) (read-eff-low) (izy-fetch-ptr-high) (fixup-page-read) (read-to-dl)
  (cycle (write ad dl) (asl dl)) (cycle (write ad dl) (ora a dl)))

;; ---------------------------------------------------------------------------
;; RLA - ROL + AND (Rotate left then AND with accumulator) - RMW unofficial
;; ---------------------------------------------------------------------------

(state rla-zp
  (fetch-zp-addr) (read-to-dl)
  (cycle (write ad dl) (rol dl)) (cycle (write ad dl) (and a dl)))

(state rla-zpx
  (fetch-zp-addr) (dummy-add-x) (read-to-dl)
  (cycle (write ad dl) (rol dl)) (cycle (write ad dl) (and a dl)))

(state rla-abs
  (fetch-adl) (fetch-adh) (read-to-dl)
  (cycle (write ad dl) (rol dl)) (cycle (write ad dl) (and a dl)))

(state rla-abx
  (fetch-adl) (fetch-adh-add-x) (fixup-page-read) (read-to-dl)
  (cycle (write ad dl) (rol dl)) (cycle (write ad dl) (and a dl)))

(state rla-aby
  (fetch-adl) (fetch-adh-add-y) (fixup-page-read) (read-to-dl)
  (cycle (write ad dl) (rol dl)) (cycle (write ad dl) (and a dl)))

(state rla-izx
  (fetch-zp-ptr) (setup-ptr-dummy) (add-x-read-low) (izx-read-ptr-high) (read-eff-low)
  (cycle (write ad dl) (rol dl)) (cycle (write ad dl) (and a dl)))

(state rla-izy
  (fetch-zp-ptr) (read-eff-low) (izy-fetch-ptr-high) (fixup-page-read) (read-to-dl)
  (cycle (write ad dl) (rol dl)) (cycle (write ad dl) (and a dl)))

;; ---------------------------------------------------------------------------
;; SRE - LSR + EOR (Shift right then XOR with accumulator) - RMW unofficial
;; ---------------------------------------------------------------------------

(state sre-zp
  (fetch-zp-addr) (read-to-dl)
  (cycle (write ad dl) (lsr dl)) (cycle (write ad dl) (eor a dl)))

(state sre-zpx
  (fetch-zp-addr) (dummy-add-x) (read-to-dl)
  (cycle (write ad dl) (lsr dl)) (cycle (write ad dl) (eor a dl)))

(state sre-abs
  (fetch-adl) (fetch-adh) (read-to-dl)
  (cycle (write ad dl) (lsr dl)) (cycle (write ad dl) (eor a dl)))

(state sre-abx
  (fetch-adl) (fetch-adh-add-x) (fixup-page-read) (read-to-dl)
  (cycle (write ad dl) (lsr dl)) (cycle (write ad dl) (eor a dl)))

(state sre-aby
  (fetch-adl) (fetch-adh-add-y) (fixup-page-read) (read-to-dl)
  (cycle (write ad dl) (lsr dl)) (cycle (write ad dl) (eor a dl)))

(state sre-izx
  (fetch-zp-ptr) (setup-ptr-dummy) (add-x-read-low) (izx-read-ptr-high) (read-eff-low)
  (cycle (write ad dl) (lsr dl)) (cycle (write ad dl) (eor a dl)))

(state sre-izy
  (fetch-zp-ptr) (read-eff-low) (izy-fetch-ptr-high) (fixup-page-read) (read-to-dl)
  (cycle (write ad dl) (lsr dl)) (cycle (write ad dl) (eor a dl)))

;; ---------------------------------------------------------------------------
;; RRA - ROR + ADC (Rotate right then add with carry) - RMW unofficial
;; ---------------------------------------------------------------------------

(state rra-zp
  (fetch-zp-addr) (read-to-dl)
  (cycle (write ad dl) (ror dl)) (cycle (write ad dl) (adc a dl)))

(state rra-zpx
  (fetch-zp-addr) (dummy-add-x) (read-to-dl)
  (cycle (write ad dl) (ror dl)) (cycle (write ad dl) (adc a dl)))

(state rra-abs
  (fetch-adl) (fetch-adh) (read-to-dl)
  (cycle (write ad dl) (ror dl)) (cycle (write ad dl) (adc a dl)))

(state rra-abx
  (fetch-adl) (fetch-adh-add-x) (fixup-page-read) (read-to-dl)
  (cycle (write ad dl) (ror dl)) (cycle (write ad dl) (adc a dl)))

(state rra-aby
  (fetch-adl) (fetch-adh-add-y) (fixup-page-read) (read-to-dl)
  (cycle (write ad dl) (ror dl)) (cycle (write ad dl) (adc a dl)))

(state rra-izx
  (fetch-zp-ptr) (setup-ptr-dummy) (add-x-read-low) (izx-read-ptr-high) (read-eff-low)
  (cycle (write ad dl) (ror dl)) (cycle (write ad dl) (adc a dl)))

(state rra-izy
  (fetch-zp-ptr) (read-eff-low) (izy-fetch-ptr-high) (fixup-page-read) (read-to-dl)
  (cycle (write ad dl) (ror dl)) (cycle (write ad dl) (adc a dl)))

;; ---------------------------------------------------------------------------
;; ANC - AND + set C from bit 7 (two opcodes: 0B and 2B)
;; ---------------------------------------------------------------------------

(state anc-imm
  (cycle (fetch dl pc))
  (cycle (and a dl) (set-c-from-n))
 )

;; ---------------------------------------------------------------------------
;; ALR/ASR - AND + LSR
;; ---------------------------------------------------------------------------

(state alr-imm
  (cycle (fetch dl pc))
  (cycle (and a dl) (lsr a))
 )

;; ---------------------------------------------------------------------------
;; ARR - AND + ROR (with special flag handling)
;; ---------------------------------------------------------------------------

(state arr-imm
  (cycle (fetch dl pc))
  (cycle (arr a dl))
 )

;; ---------------------------------------------------------------------------
;; XAA/ANE - TXA + AND (unstable)
;; ---------------------------------------------------------------------------

(state xaa-imm
  (cycle (fetch dl pc))
  (cycle (mov a x) (and a dl))
 )

;; ---------------------------------------------------------------------------
;; AXS/SBX - (A & X) - imm -> X (no borrow)
;; ---------------------------------------------------------------------------

(state axs-imm
  (cycle (fetch dl pc))
  (cycle (axs x dl))
 )

;; ---------------------------------------------------------------------------
;; STP/KIL/JAM - Halt the CPU (we just infinite loop)
;; ---------------------------------------------------------------------------

(state stp
  (cycle (dummy pc))
  (goto stp))

;; ---------------------------------------------------------------------------
;; Unofficial NOPs (various byte lengths)
;; ---------------------------------------------------------------------------

;; 1-byte NOPs (implied)
(state nop-1
  (cycle (dummy pc))
 )

;; 2-byte NOPs (skip one byte)
(state nop-2
  (cycle (fetch dl pc))
  (cycle (dummy pc))
 )

;; 3-byte NOPs (skip two bytes)
(state nop-3
  (cycle (fetch dl pc))
  (cycle (fetch dl pc))
  (cycle (dummy pc))
 )

;; 2-byte NOP with zero page read
(state nop-zp
  (cycle (fetch adl pc) (set-adh-zero))
  (cycle (read dl ad))
  (cycle (dummy pc))
 )

;; 2-byte NOP with zero page,X read
(state nop-zpx
  (cycle (fetch adl pc) (set-adh-zero))
  (cycle (dummy ad) (add8-latch-carry adl adl x))
  (cycle (read dl ad))
  (cycle (dummy pc))
 )

;; 3-byte NOP with absolute read
(state nop-abs
  (fetch-adl) (fetch-adh) (read-to-dl) (dummy-read-pc))

;; 3-byte NOP with absolute,X read (with page cross)
(state nop-abx
  (fetch-adl) (fetch-adh-add-x) (read-to-dl)
  (when page-cross (fixup-page-read))
  (dummy-read-pc))

;; ---------------------------------------------------------------------------
;; LAS/LAR - Load A, X, SP from (memory & SP)
;; ---------------------------------------------------------------------------

(state las-aby
  (fetch-adl) (fetch-adh-add-y) (read-to-dl)
  (when page-cross (fixup-page-read))
  (cycle (las-op)))

;; ---------------------------------------------------------------------------
;; TAS/SHS - Transfer A & X to SP, store A & X & (high+1)
;; On page cross, address is corrupted to (ADH+1) & A & X
;; tas-fixup-page sets SP = A & X, computes DL = SP & (ADH+1), corrupts ADH
;; ---------------------------------------------------------------------------

(state tas-aby
  (fetch-adl) (fetch-adh-add-y) (tas-fixup-page)
  (write-from-dl))

;; ---------------------------------------------------------------------------
;; SHA/AHX - Store A & X & (high+1)
;; On page cross, address is corrupted to (ADH+1) & A & X
;; sha-fixup-page computes DL = A & X & (ADH+1) and corrupts ADH
;; ---------------------------------------------------------------------------

(state sha-aby
  (fetch-adl) (fetch-adh-add-y) (sha-fixup-page)
  (write-from-dl))

(state sha-izy
  (fetch-zp-ptr) (read-eff-low) (izy-fetch-ptr-high) (sha-fixup-page)
  (write-from-dl))

;; ---------------------------------------------------------------------------
;; SHX/SXA - Store X & (high+1)
;; On page cross, address is corrupted to (ADH+1) & X
;; shx-fixup-page computes DL = X & (ADH+1) and corrupts ADH
;; ---------------------------------------------------------------------------

(state shx-aby
  (fetch-adl) (fetch-adh-add-y) (shx-fixup-page)
  (write-from-dl))

;; ---------------------------------------------------------------------------
;; SHY/SYA - Store Y & (high+1)
;; On page cross, address is corrupted to (ADH+1) & Y
;; shy-fixup-page computes DL = Y & (ADH+1) and corrupts ADH
;; ---------------------------------------------------------------------------

(state shy-abx
  (fetch-adl) (fetch-adh-add-x) (shy-fixup-page)
  (write-from-dl))

;; =============================================================================
;; OPCODE MAPPINGS (all 256 opcodes)
;; =============================================================================

;; Row #x00
(opcode #x00 brk)
(opcode #x01 ora-izx)
(opcode #x02 stp)
(opcode #x03 slo-izx)
(opcode #x04 nop-zp)
(opcode #x05 ora-zp)
(opcode #x06 asl-zp)
(opcode #x07 slo-zp)
(opcode #x08 php)
(opcode #x09 ora-imm)
(opcode #x0A asl-acc)
(opcode #x0B anc-imm)
(opcode #x0C nop-abs)
(opcode #x0D ora-abs)
(opcode #x0E asl-abs)
(opcode #x0F slo-abs)

;; Row #x10
(opcode #x10 bpl)
(opcode #x11 ora-izy)
(opcode #x12 stp)
(opcode #x13 slo-izy)
(opcode #x14 nop-zpx)
(opcode #x15 ora-zpx)
(opcode #x16 asl-zpx)
(opcode #x17 slo-zpx)
(opcode #x18 clc)
(opcode #x19 ora-aby)
(opcode #x1A nop-1)
(opcode #x1B slo-aby)
(opcode #x1C nop-abx)
(opcode #x1D ora-abx)
(opcode #x1E asl-abx)
(opcode #x1F slo-abx)

;; Row #x20
(opcode #x20 jsr)
(opcode #x21 and-izx)
(opcode #x22 stp)
(opcode #x23 rla-izx)
(opcode #x24 bit-zp)
(opcode #x25 and-zp)
(opcode #x26 rol-zp)
(opcode #x27 rla-zp)
(opcode #x28 plp)
(opcode #x29 and-imm)
(opcode #x2A rol-acc)
(opcode #x2B anc-imm)
(opcode #x2C bit-abs)
(opcode #x2D and-abs)
(opcode #x2E rol-abs)
(opcode #x2F rla-abs)

;; Row #x30
(opcode #x30 bmi)
(opcode #x31 and-izy)
(opcode #x32 stp)
(opcode #x33 rla-izy)
(opcode #x34 nop-zpx)
(opcode #x35 and-zpx)
(opcode #x36 rol-zpx)
(opcode #x37 rla-zpx)
(opcode #x38 sec)
(opcode #x39 and-aby)
(opcode #x3A nop-1)
(opcode #x3B rla-aby)
(opcode #x3C nop-abx)
(opcode #x3D and-abx)
(opcode #x3E rol-abx)
(opcode #x3F rla-abx)

;; Row #x40
(opcode #x40 rti)
(opcode #x41 eor-izx)
(opcode #x42 stp)
(opcode #x43 sre-izx)
(opcode #x44 nop-zp)
(opcode #x45 eor-zp)
(opcode #x46 lsr-zp)
(opcode #x47 sre-zp)
(opcode #x48 pha)
(opcode #x49 eor-imm)
(opcode #x4A lsr-acc)
(opcode #x4B alr-imm)
(opcode #x4C jmp-abs)
(opcode #x4D eor-abs)
(opcode #x4E lsr-abs)
(opcode #x4F sre-abs)

;; Row #x50
(opcode #x50 bvc)
(opcode #x51 eor-izy)
(opcode #x52 stp)
(opcode #x53 sre-izy)
(opcode #x54 nop-zpx)
(opcode #x55 eor-zpx)
(opcode #x56 lsr-zpx)
(opcode #x57 sre-zpx)
(opcode #x58 cli)
(opcode #x59 eor-aby)
(opcode #x5A nop-1)
(opcode #x5B sre-aby)
(opcode #x5C nop-abx)
(opcode #x5D eor-abx)
(opcode #x5E lsr-abx)
(opcode #x5F sre-abx)

;; Row #x60
(opcode #x60 rts)
(opcode #x61 adc-izx)
(opcode #x62 stp)
(opcode #x63 rra-izx)
(opcode #x64 nop-zp)
(opcode #x65 adc-zp)
(opcode #x66 ror-zp)
(opcode #x67 rra-zp)
(opcode #x68 pla)
(opcode #x69 adc-imm)
(opcode #x6A ror-acc)
(opcode #x6B arr-imm)
(opcode #x6C jmp-ind)
(opcode #x6D adc-abs)
(opcode #x6E ror-abs)
(opcode #x6F rra-abs)

;; Row #x70
(opcode #x70 bvs)
(opcode #x71 adc-izy)
(opcode #x72 stp)
(opcode #x73 rra-izy)
(opcode #x74 nop-zpx)
(opcode #x75 adc-zpx)
(opcode #x76 ror-zpx)
(opcode #x77 rra-zpx)
(opcode #x78 sei)
(opcode #x79 adc-aby)
(opcode #x7A nop-1)
(opcode #x7B rra-aby)
(opcode #x7C nop-abx)
(opcode #x7D adc-abx)
(opcode #x7E ror-abx)
(opcode #x7F rra-abx)

;; Row #x80
(opcode #x80 nop-2)
(opcode #x81 sta-izx)
(opcode #x82 nop-2)
(opcode #x83 sax-izx)
(opcode #x84 sty-zp)
(opcode #x85 sta-zp)
(opcode #x86 stx-zp)
(opcode #x87 sax-zp)
(opcode #x88 dey)
(opcode #x89 nop-2)
(opcode #x8A txa)
(opcode #x8B xaa-imm)
(opcode #x8C sty-abs)
(opcode #x8D sta-abs)
(opcode #x8E stx-abs)
(opcode #x8F sax-abs)

;; Row #x90
(opcode #x90 bcc)
(opcode #x91 sta-izy)
(opcode #x92 stp)
(opcode #x93 sha-izy)
(opcode #x94 sty-zpx)
(opcode #x95 sta-zpx)
(opcode #x96 stx-zpy)
(opcode #x97 sax-zpy)
(opcode #x98 tya)
(opcode #x99 sta-aby)
(opcode #x9A txs)
(opcode #x9B tas-aby)
(opcode #x9C shy-abx)
(opcode #x9D sta-abx)
(opcode #x9E shx-aby)
(opcode #x9F sha-aby)

;; Row #xA0
(opcode #xA0 ldy-imm)
(opcode #xA1 lda-izx)
(opcode #xA2 ldx-imm)
(opcode #xA3 lax-izx)
(opcode #xA4 ldy-zp)
(opcode #xA5 lda-zp)
(opcode #xA6 ldx-zp)
(opcode #xA7 lax-zp)
(opcode #xA8 tay)
(opcode #xA9 lda-imm)
(opcode #xAA tax)
(opcode #xAB lax-imm)
(opcode #xAC ldy-abs)
(opcode #xAD lda-abs)
(opcode #xAE ldx-abs)
(opcode #xAF lax-abs)

;; Row #xB0
(opcode #xB0 bcs)
(opcode #xB1 lda-izy)
(opcode #xB2 stp)
(opcode #xB3 lax-izy)
(opcode #xB4 ldy-zpx)
(opcode #xB5 lda-zpx)
(opcode #xB6 ldx-zpy)
(opcode #xB7 lax-zpy)
(opcode #xB8 clv)
(opcode #xB9 lda-aby)
(opcode #xBA tsx)
(opcode #xBB las-aby)
(opcode #xBC ldy-abx)
(opcode #xBD lda-abx)
(opcode #xBE ldx-aby)
(opcode #xBF lax-aby)

;; Row #xC0
(opcode #xC0 cpy-imm)
(opcode #xC1 cmp-izx)
(opcode #xC2 nop-2)
(opcode #xC3 dcp-izx)
(opcode #xC4 cpy-zp)
(opcode #xC5 cmp-zp)
(opcode #xC6 dec-zp)
(opcode #xC7 dcp-zp)
(opcode #xC8 iny)
(opcode #xC9 cmp-imm)
(opcode #xCA dex)
(opcode #xCB axs-imm)
(opcode #xCC cpy-abs)
(opcode #xCD cmp-abs)
(opcode #xCE dec-abs)
(opcode #xCF dcp-abs)

;; Row #xD0
(opcode #xD0 bne)
(opcode #xD1 cmp-izy)
(opcode #xD2 stp)
(opcode #xD3 dcp-izy)
(opcode #xD4 nop-zpx)
(opcode #xD5 cmp-zpx)
(opcode #xD6 dec-zpx)
(opcode #xD7 dcp-zpx)
(opcode #xD8 cld)
(opcode #xD9 cmp-aby)
(opcode #xDA nop-1)
(opcode #xDB dcp-aby)
(opcode #xDC nop-abx)
(opcode #xDD cmp-abx)
(opcode #xDE dec-abx)
(opcode #xDF dcp-abx)

;; Row #xE0
(opcode #xE0 cpx-imm)
(opcode #xE1 sbc-izx)
(opcode #xE2 nop-2)
(opcode #xE3 isc-izx)
(opcode #xE4 cpx-zp)
(opcode #xE5 sbc-zp)
(opcode #xE6 inc-zp)
(opcode #xE7 isc-zp)
(opcode #xE8 inx)
(opcode #xE9 sbc-imm)
(opcode #xEA nop)
(opcode #xEB sbc-imm)
(opcode #xEC cpx-abs)
(opcode #xED sbc-abs)
(opcode #xEE inc-abs)
(opcode #xEF isc-abs)

;; Row #xF0
(opcode #xF0 beq)
(opcode #xF1 sbc-izy)
(opcode #xF2 stp)
(opcode #xF3 isc-izy)
(opcode #xF4 nop-zpx)
(opcode #xF5 sbc-zpx)
(opcode #xF6 inc-zpx)
(opcode #xF7 isc-zpx)
(opcode #xF8 sed)
(opcode #xF9 sbc-aby)
(opcode #xFA nop-1)
(opcode #xFB isc-aby)
(opcode #xFC nop-abx)
(opcode #xFD sbc-abx)
(opcode #xFE inc-abx)
(opcode #xFF isc-abx)
