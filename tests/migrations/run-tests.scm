;; Test-suite runner.
;;
;; Copyright (C) 2016 Hasanur Rahevy
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

(define tests (filter (lambda (arg) (not (string-prefix? arg "--"))) *args*))

(run-tests (if (null? tests)
	       (load-tests "tests" "migrations")
	       (map (lambda (name)
		      (test::scm #f
			         #f
				 (path-join "tests" "migrations" name)
				 (in-srcdir "tests" "migrations" name))) tests)))
