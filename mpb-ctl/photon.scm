; Copyright (C) 1999 Massachusetts Institute of Technology.
;
; This program is free software; you can redistribute it and/or modify
; it under the terms of the GNU General Public License as published by
; the Free Software Foundation; either version 2 of the License, or
; (at your option) any later version.
;
; This program is distributed in the hope that it will be useful,
; but WITHOUT ANY WARRANTY; without even the implied warranty of
; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
; GNU General Public License for more details.
;
; You should have received a copy of the GNU General Public License
; along with this program; if not, write to the Free Software
; Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

; ****************************************************************

(define-class material-type no-parent
  (define-property epsilon 'number no-default positive?))

; Define an alias, "dielectric" for "material-type".
(define dielectric material-type)

; use the solid geometry classes, variables, etcetera in libgeom:
; (one specifications file can include another specifications file)
(include "../../libctl/utils/libgeom/geom.scm")

; ****************************************************************

; More input/output variables (besides those defined by libgeom, above).

(define-input-var k-points '() (make-list-type 'vector3))

(define-input-var num-bands 1 'integer)
(define-input-var tolerance 1.0e-4 'number positive?)
(define-input-var target-freq 0.0 'number (lambda (x) (>= x 0)))

(define-input-var grid-size (vector3 16 16 16) 'vector3
  (lambda (v) (vector-for-all? v (lambda (x) (and (> x 0) (integer? x))))))
(define-input-var mesh-size 7 'integer positive?)

(define-output-var freqs (make-list-type 'number))

; ****************************************************************

; Definitions of external (C) functions:

; (init-params true) initializes the geometry, etcetera, and does everything
; else that's needed to get ready for an eigenvalue calculation.
; This should be called after the input variables are changed.
; If false is passed instead of true, fields from a previous run are
; retained, if possible, as a starting point for the eigensolver.
(define-external-function init-params true false no-return-value 'boolean)

; (set-polarization p) sets the polarization that is solved for by
; solve-kpoint, below.  p should be one of the constants NO-POLARIZATION,
; TE, or TM (the default for solve-kpoint is NO-POLARIZATION).  
; init-params should already have been called.
(define-external-function set-polarization false false 
  no-return-value 'integer)
(define NO-POLARIZATION 0)
(define TE 1)
(define TM 2)
(define PREV-POLARIZATION 3)

; (solve-kpoint kpoint) solves for the specified bands at the given k point.
; Requires that (init-params) has been called, and does not re-read the
; input variables, but does write the output vars.
(define-external-function solve-kpoint false true no-return-value 'vector3)

(define-external-function get-dfield false false no-return-value 'integer)
(define-external-function get-hfield false false no-return-value 'integer)
(define-external-function get-efield-from-dfield false false no-return-value)
(define-external-function get-epsilon false false no-return-value)
(define-external-function compute-field-energy false false no-return-value)
(define-external-function compute-energy-in-dielectric false false
  'number 'number 'number)
(define-external-function output-field-extended false false
  no-return-value 'vector3)  
(define-external-function compute-energy-in-object-list false false
  'number (make-list-type 'geometric-object))

; ****************************************************************

; Add some predefined variables, for convenience:

(define vacuum (make material-type (epsilon 1.0)))
(define air vacuum)

(define infinity 1.0e20) ; big number for infinite dimensions of objects

(set! default-material air)

; ****************************************************************

; The remainder of this file consists of Scheme convenience functions.

; ****************************************************************

; functions to manipulate the fields; these are mainly convenient
; wrappers for the external functions defined previously.

(define (get-efield which-band)
  (get-dfield which-band)
  (get-efield-from-dfield))

(define (output-field . copies)
  (output-field-extended (apply vector3 copies)))

(define (output-epsilon . copies)
  (get-epsilon)
  (apply output-field copies))

(define (compute-energy-in-objects . objects)
  (compute-energy-in-object-list objects))

; ****************************************************************
; Functions to compute and output gaps, given the lists of frequencies
; computed at each k point.

; The band-range-data is a list if (min . max) pairs, with each pair
; describing the frequency range of a band.  Here, we update this data
; with a new list of band frequencies, and return the new data.  If
; band-range-data is null or too short, the needed entries will be
; created.
(define (update-band-range-data band-range-data freqs)
  (if (null? freqs)
      '()
      (let ((br (if (null? band-range-data)
		    (cons infinity (- infinity))
		    (car band-range-data)))
	    (br-rest (if (null? band-range-data) '() (cdr band-range-data))))
	(cons (cons (min (car freqs) (car br))
		    (max (car freqs) (cdr br)))
	      (update-band-range-data br-rest (cdr freqs))))))

; Output any gaps in the given band ranges, and return a list
; of the gaps as a list of (percent freq-min freq-max) lists.
(define (output-gaps band-range-data)
  (define (ogaps br-cur br-rest i)
    (if (null? br-rest)
	'()
	(if (>= (cdr br-cur) (caar br-rest))
	    (ogaps (car br-rest) (cdr br-rest) (+ i 1))
	    (let ((gap-size (/ (* 200 (- (caar br-rest) (cdr br-cur)))
			       (+ (caar br-rest) (cdr br-cur)))))
	      (display-many "Gap from band " i " (" (cdr br-cur)
			    ") to band " (+ i 1) " (" (caar br-rest) "), "
			    gap-size "%\n")
	      (cons (list gap-size (cdr br-cur) (caar br-rest))
		    (ogaps (car br-rest) (cdr br-rest) (+ i 1)))))))
  (if (null? band-range-data)
      '()
      (ogaps (car band-range-data) (cdr band-range-data) 1)))

; variable holding the current list of gaps, in the format returned
; by output-gaps, above
(define gap-list '())

; ****************************************************************

; (run) functions, to do vanilla calculations.  They all take zero or
; more "band functions."  Each function should take a single
; parameter, the band index, and is called for each band index at
; every k point.  These are typically used to output the bands.

(define (run-polarization p . band-functions)
  (define band-range-data '())
  (set! interactive false)  ; don't be interactive if we call (run)
  (init-params true)
  (set-polarization p)
  (output-epsilon)          ; output epsilon immediately
  (map (lambda (k)
	 (solve-kpoint k)
	 (set! band-range-data (update-band-range-data band-range-data freqs))
	 (map (lambda (f)
		(do ((band 0 (+ band 1))) ((= band num-bands)) (f band)))
	      band-functions))
       k-points)
  (set! gap-list (output-gaps band-range-data))
  (display "done.\n"))

(define (run . band-functions)
  (apply run-polarization (cons NO-POLARIZATION band-functions)))

(define (run-te . band-functions)
  (apply run-polarization (cons TE band-functions)))

(define (run-tm . band-functions)
  (apply run-polarization (cons TM band-functions)))

; ****************************************************************

; Some predefined output functions (functions of the band index),
; for passing to (run).

(define (output-hfield which-band)
  (get-hfield which-band)
  (output-field))

(define (output-dfield which-band)
  (get-dfield which-band)
  (output-field))

(define (output-efield which-band)
  (get-efield which-band)
  (output-field))

(define (output-hpwr which-band)
  (get-hfield which-band)
  (compute-field-energy)
  (output-field))

(define (output-dpwr which-band)
  (get-dfield which-band)
  (compute-field-energy)
  (output-field))

; The following function returns an output function that calls
; output-func for bands with D energy in objects > min-energy.
; For example, (output-dpwr-in-objects output-dfield 0.20 some-object)
; would return an output function that would spit out the D field
; for bands with at least %20 of their D energy in some-object.
(define (output-dpwr-in-objects output-func min-energy . objects)
  (lambda (which-band)
    (get-dfield which-band)
    (compute-field-energy)
    (let ((energy (compute-energy-in-object-list objects)))
        ; output the computed energy for grepping:
	(display-many "dpwr:, " which-band ", " (list-ref freqs which-band)
		      ", " energy "\n")
	(if (>= energy min-energy)
	    (output-func which-band)))))

; ****************************************************************