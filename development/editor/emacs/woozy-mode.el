;;; woozy-mode.el --- Woozy major mode (woozy-editor-setup) -*- lexical-binding: t; -*-

;;; Commentary:
;; Font locking and comment commands for Woozy source files.

;;; Code:

(defvar woozy-mode-syntax-table
  (let ((table (make-syntax-table)))
    (dolist (char (string-to-list "+-=%!~&|^<>.,:;"))
      (modify-syntax-entry char "." table))
    (modify-syntax-entry ?_ "_" table)
    (modify-syntax-entry ?/ ". 124b" table)
    (modify-syntax-entry ?* ". 23" table)
    (modify-syntax-entry ?\n "> b" table)
    (modify-syntax-entry ?\" "|" table)
    (modify-syntax-entry ?` "\"" table)
    (modify-syntax-entry ?\\ "." table)
    table)
  "Syntax table for `woozy-mode'.")

(defun woozy--syntax-propertize (start end)
  "Mark escapes and line endings in double-quoted strings from START to END."
  (goto-char start)
  (while (re-search-forward "[\\\\\n]" end t)
    (let* ((pos (match-beginning 0))
           (state (save-excursion (syntax-ppss pos))))
      (when (and (eq (nth 3 state) t) (not (nth 5 state)))
        ;; A newline ends an unfinished double-quoted string.
        (put-text-property pos (1+ pos) 'syntax-table
                           (string-to-syntax
                            (if (eq (char-after pos) ?\n) "|" "\\")))))))

(defconst woozy--font-lock-keywords
  `((,(regexp-opt
       '("as" "async" "await" "break" "case" "catch" "const" "continue"
         "default" "defer" "else" "enum" "for" "func" "if" "impl" "import"
         "in" "inout" "let" "move" "package" "pub" "return" "struct" "switch"
         "throw" "throws" "trait" "try" "unsafe" "var")
       'symbols)
     . font-lock-keyword-face)
    (,(regexp-opt '("false" "true") 'symbols) . font-lock-constant-face)
    (,(regexp-opt '("class" "extends" "extern" "operator") 'symbols)
     . font-lock-warning-face)
    (,(regexp-opt
       '("Unit" "bool" "i8" "i16" "i32" "i64" "u8" "u16" "u32" "u64"
         "isize" "usize" "f32" "f64" "string" "Self" "Option" "Vec" "Slice" "Weak" "Task"
         "Result" "Ptr" "PtrMut" "Buffer" "BufferMut" "__woozy_Thread"
         "__woozy_Mutex" "__woozy_Condvar" "__woozy_AtomicI64"
         "__woozy_IpcHandle")
       'symbols)
     . font-lock-type-face)
    (,(rx symbol-start "func" (+ blank)
          (group (any "A-Za-z_") (* (any "A-Za-z_0-9"))))
     1 font-lock-function-name-face)
    (,(rx symbol-start (or "struct" "enum" "trait") (+ blank)
          (group (any "A-Za-z_") (* (any "A-Za-z_0-9"))))
     1 font-lock-type-face)
    (,(rx symbol-start
          (or (seq "0b" (any "01") (* (? "_") (any "01")))
              (seq "0o" (any "0-7") (* (? "_") (any "0-7")))
              (seq "0x" hex-digit (* (? "_") hex-digit))
              (seq (or "0" (seq (any "1-9") (* (? "_") digit)))
                   (? "." digit (* (? "_") digit))
                   (? (any "eE") (? (any "+-")) digit (* (? "_") digit))))
          symbol-end)
     . font-lock-constant-face))
  "Font-lock rules for `woozy-mode'.")

;;;###autoload
(define-derived-mode woozy-mode prog-mode "Woozy"
  "Major mode for editing Woozy source files."
  (setq-local font-lock-defaults '(woozy--font-lock-keywords))
  (setq-local syntax-propertize-function #'woozy--syntax-propertize)
  (setq-local comment-start "// ")
  (setq-local comment-end "")
  (setq-local comment-start-skip "//+[ \t]*"))

;;;###autoload
(add-to-list 'auto-mode-alist '("\\.wz\\'" . woozy-mode))

(provide 'woozy-mode)
;;; woozy-mode.el ends here
