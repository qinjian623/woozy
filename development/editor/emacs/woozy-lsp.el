;;; woozy-lsp.el --- woozy-editor-setup managed file -*- lexical-binding: t; -*-

(require 'cl-lib)
(require 'project)
(load (expand-file-name "woozy-mode.el" (file-name-directory load-file-name))
      nil 'nomessage)
(autoload 'eglot-ensure "eglot")

(with-eval-after-load 'eglot
  (add-to-list 'eglot-server-programs
               '(woozy-mode . (@WOOZYC@ "lsp"))))

(defun woozy--find-project (directory)
  (when-let* ((root (locate-dominating-file directory "lang.toml")))
    (cons 'woozy root)))

(cl-defmethod project-root ((project (head woozy)))
  (cdr project))

(defun woozy--start-eglot ()
  (when (and buffer-file-name (not (file-remote-p buffer-file-name)))
    (add-hook 'project-find-functions #'woozy--find-project nil t)
    (eglot-ensure)))

(add-hook 'woozy-mode-hook #'woozy--start-eglot)

(provide 'woozy-lsp)
