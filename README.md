The project was made with the electronic tuning radio, without any features (like the casette player, which obviously made it easier because there is not any holes on the face plate, or i dont have to make the player work. Tho with this design it should be very simple to implement it, because it only 2 inputs of the 4 input analog multiplexer.)
This project is almost entirely run by claude (but chatgpt and gemini is unusable for coding stuff like this, i know that they have more usage for free). Before you ask, i am an electronics technician student, but i dont learn stuff like this in school (or just not yet). 
The goal of the project is that i get to do some electronic stuff, while i learn some new stuff. The main reason why i started using claude by default, even helping me with the parts list, is just i dont have any experience in any electronic stuff yet (i do know how to apply all of Kirchoffs' law tho luckily). Realistically i only made this repo, to put it in my project's context for claude so i dont have to always send it the current code, for it to rewrite. I come up with the ideas and claude just does them. Tho if you have actual gemini pro it can also get some useable code in 2-3 promtps too, but claude is just making way less mistakes overall.

The fully finished radio uses the olds housing, the old everything, that is why i didn't just buy an aftermarket bluetooth delco radio. Also the new radio can also do time syncing based on wifi, and the radio signlas, so i dont have to touch it ever again, which is good, because if the car sits in the garage for multiple months during winter i dont have to tell it the time again, and again. The goal of the project is that the ESP can use and control the 
    1. Volume
    2. Fade/Tune (front/rear balance)
    3. Switching between radio and bluetooth
    4. Stopping bluetooth when you "turn off" the radio
    5. Left/right balance with bluetooth

I think i will upload the kicad file, and if i manage to do it, a gerber for the custom pcb for the whole project.

The full parts list that i used is: (if it seems like multiple modules when you search them, it can be because i use a multiple part board. I bought my stuff from hestore.hu, maybe you can use it too)
    - the two potentiometers found in a 1984 olds.
    - the old antenna connector
    - the old housing, with all buttons.
    - ESP32 wroom (DEVKIT-V1), (i used this because newer esp-s usually only have ble, so you cant use them for bluetooth playback)
    - CJMCU 1334
    - TDA 7388
    - RDA 5807M (the little green premade board with the cristal already in its place)
    - TM 1637-M4C36-RD1
    - DS3231+AT24C32
    - CD4052B
    - ICL7660
    
    - Capacitors:     - Electrolit:    - 47 uF
                                        - 470 uF
                                        - 10 uF
                    - Film capacitor:    - 100 nF
                                        - 0.47 uF
                                        - 0.1 uF
                                        - 1 uF
                                        
    - Resistors:    - 47k
I recommend buying atleast a 10-pack of everything.
