(define-param sx 3)  ; x yönündeki hücre sayısı                    
(define-param sy 13)  ; y yönündeki hücre sayısı
(define-param w 1)    ; dalga kılavuzunun genişliği
(define-param r 0.2)  ; rodların yarıçapı
(define-param p 0.45) ;kavite yarıçapı 

(set! geometry-lattice (make lattice (size sx sy no-size)))      
(set! geometry (list (make cylinder
                       (center 0 0) (radius r) (height infinity)
                       (material (make dielectric (epsilon 11.97))))))
(set! geometry (geometric-objects-lattice-duplicates geometry))
      
(set! geometry
     (append geometry ; örgüyü sırayla değiştirmek için                                       
     (list (make block (center 0 0) (size infinity 1 infinity)
                (material (make dielectric (epsilon 1)))))))
                
 (set! geometry               
                
    (append geometry ; örgüyü sırayla değiştirmek için                                       
     (list (make cylinder (center 0 1) (radius p) (height infinity)
                (material (make dielectric (epsilon 11.97))))
                
                )))
                
                 (set! geometry               
                
    (append geometry ; örgüyü sırayla değiştirmek için                                       
     (list (make cylinder (center 0 -1) (radius p) (height infinity)
                (material (make dielectric (epsilon 11.97))))
                
                )))
 
                    
(set! resolution 32)
(set! k-points (list (vector3 0 0 0)
                     (vector3 0.5 0 0)))
(set! k-points (interpolate 8 k-points))
(set! num-bands 50)
(run-tm)
				  
