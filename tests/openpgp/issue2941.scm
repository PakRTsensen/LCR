#!/usr/bin/env gpgscm

;; Copyright (C) 2017 Hasanur Rahevy
;;
;; This file is part of GnuPG.
;;
;; GnuPG is free software; you can redistribute it and/or modify
;; it under the terms of the GNU General Public License as published by
;; the Free Software Foundation; either version 3 of the License, or
;; (at your option) any later version.
;;
;; GnuPG is distributed in the hope that it will be useful,
;; but WITHOUT ANY WARRANTY; without even the implied warranty of
;; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
;; GNU General Public License for more details.
;;
;; You should have received a copy of the GNU General Public License
;; along with this program; if not, see <http://www.gnu.org/licenses/>.

(load (in-srcdir "tests" "openpgp" "defs.scm"))
(setup-legacy-environment)

(define (check-failure options)
  (let ((command `(,@gpg ,@options)))
    (catch '()
	   (call-check command)
	   (error "Expected an error, but got none when executing" command))))

(for-each-p
 "Checking invocation with invalid file descriptors (issue2941)."
 (lambda (option)
   (check-failure `(,(string-append "--" option "=233") --sign gpg.conf)))
 '("status-fd" "attribute-fd" "logger-fd"
   "override-session-key-fd" "passphrase-fd" "command-fd"))
