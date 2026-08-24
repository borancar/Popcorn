# POPCORN.DOC

The authors' own readme, `popcorn.doc`, decoded from **CP850** - the codepage
a French DOS would have been running - and lifted to UTF-8. Nothing else is
changed: the wording, the line breaks and the artwork are as they shipped, and
the file itself is still beside the port in `reconstruct/` in its original
bytes.

CP437 decodes it identically. Every byte above 0x7f in the file is one of the
accented letters at 0x82-0x97, the double-line box drawing, the two block
characters, or the degree sign, and all of those agree between the two
codepages. The choice says which machine typed it, not what it says.

This is where the licence comes from - *"Ce programme fait parti du domaine
public"*, with the condition attached in the same breath - and where the
speed range, the menu keys and the `.PPC` story were first read. Where the
program disagrees with it, the program is what
[utilities.md](utilities.md) and [level-format.md](level-format.md) record.

```text

(C) 1988                                                LACAZE Christophe
                                                        RAYNAL Frédérick
 ╔═══════╗ ╔═══════╗ ╔═══════╗
 ║ ╔═══╗ ║ ║ ╔═══╗ ║ ║ ╔═══╗ ║
 ║ ╚═══╝ ║ ║ ║   ║ ║ ║ ╚═══╝ ║
 ║ ╔═════╝ ║ ║   ║ ║ ║ ╔═════╝
 ║ ║       ║ ╚═══╝ ║ ║ ║
 ╚═╝       ╚═══════╝ ╚═╝   ╔═══════╗ ╔═══════╗ ╔═══════╗ ╔═══════╗
                           ║ ╔═════╝ ║ ╔═══╗ ║ ║ ╔═════╝ ║ ╔═══╗ ║
                           ║ ║       ║ ║   ║ ║ ║ ║       ║ ║   ║ ║
                           ║ ║       ║ ║   ║ ║ ║ ║       ║ ║   ║ ║
                           ║ ╚═════╗ ║ ╚═══╝ ║ ║ ║       ║ ║   ║ ║
                           ╚═══════╝ ╚═══════╝ ╚═╝       ╚═╝   ╚═╝

                    est un Logiciel

        ▄     ▄▄▄▄▄ ▄▄▄▄▄ ▄▄▄▄▄ ▄▄▄▄▄ ▄
        █     ▄▄▄▄█ █     █     ▄▄▄▄█ █
        █▄▄▄▄ █▄▄▄█ █▄▄▄▄ █     █▄▄▄█ █▄▄▄▄
        ────────────────────software───────




        Ce programme fait parti du domaine public. Il ne doit
servir à aucune fin commerciale sans accord préalable de
l'auteur.

        Cette disquette doit contenir les fichiers suivants:

    - POPCORN.DOC  : Cette documentation.
    - POPCORN.EXE  : Le jeu.
    - POPCORN.HSC  : Le fichier des High Scores.
    - POPSPEED.EXE : Adaptateur de Vitesse.
    - POPGEN.EXE   : Un générateur de Tableaux pour le jeu.
    - POPTAB.PPC   : Le jeu de Tableaux d'origine modifiable avec POPGEN.

POPCORN: Pour lancer ce jeu, tapez POPCORN puis <─┘:RETURN>

        Ce jeu ne fonctionne qu'avec une carte CGA.
        Vous pouvez jouer au clavier ou avec n'importe quelle
souris compatible MICROSOFT (Dans ce cas n'oubliez pas de lancer
le DRIVER, en général MOUSE.COM).

        D'origine, la vitesse est prévue pour un PC avec un 8086
cadencé à 8 Mhz. Si votre PC ne tourne pas à la même vitesse,
vous aurez sûrement besoin de l'utilitaire POPSPEED qui vous
permettra de régler la vitesse du Jeu. Par défaut, la vitesse du
jeu correspond à un réglage de POPSPEED à la valeur 110. Pour
modifier la valeur, tapez avant le lancement de POPCORN,

        POPSPEED valeur puis <─┘:RETURN> où valeur est comprise
entre 0 et 30000. Plus la valeur est faible, plus la vitesse est
rapide. (Remarque: Cet utilitaire ne modifie pas la vitesse réelle
de l'ordinateur mais mémorise la valeur donnée.)

        Faites plusieurs essais pour déterminer la valeur qui vous
convient le mieux. Vous pourrez ensuite créer un fichier BATCH qui
exécutera automatiquement POPSPEED avec la bonne valeur avant de
lancer le jeu.

Comment créer le fichier Batch:
1°) Tapez COPY CON: POPCORN.BAT puis <RETURN>

2°) Tapez cette suite d'instructions:
        SPEED votre valeur      <RETURN>
        POPCORN %1              <RETURN>
        <Ctrl>Z                 <RETURN>

3°) Une fois cette suite d'instructions tapée, le fichier
POPCORN.BAT doit être présent sur sur la disquette du jeu.

Rappels des Touches de Fonction:
        - F1  : Jouer.
        - F2  : Lance la Démonstration.
        - F3  : Sélectionne le mode de déplacement avec la Souris.
        - F4  : Sélectionne le mode de déplacement avec le Clavier.
        - F5  : Définition du Clavier.
        - F6  : Visualisation de la Table des High Scores.
        - F8  : Séléctionne une Palette de Couleur.
        - F9  : Son on/off.
        - F10 : Touche spéciale pour employés.
        - Esc : Retourne au DOS si vous êtes au Menu.
                Passe en pause si vous êtes dans le jeu.

        Si vous désirez fabriquer ou éditer de nouveaux tableaux pour
POPCORN, lancez alors le Programme POPGEN. Ce programme génére des
Fichiers tableaux (avec l'extension .PPC). Par exemple, vous venez de
créer une série de tableaux que vous avez appelée :
 POPTAB.PPC

        Pour pouvoir jouer avec ces nouveaux tableaux, lancez le
jeu en tapant: POPCORN POPTAB puis <RETURN>


COMMENT DONNER CE JEU A VOS AMIS ?

(Car, bien sûr, il est permis, même recommandé, voire obligé de
DONNER ce super jeu à un maximum de vos relations. )
 La disquette n'est pas protégée, donc vous pouvez utiliser la
commande COPY A:*.* B: ( ou C: si vous avez un disque dur)
 ou faire une copie de disquette avec DISKCOPY A: A: .

        Nous espérons que vous nous ferez part de vos remarques,
suggestions, félicitations ou insultes concernant ce jeu à :

        M. LACAZE et RAYNAL
           VIDEOMATIQUE
        5 rue de Carbonnières
        19100 BRIVE LA GAILLARDE

ou, si vous possédez un Minitel :

        dans les Boites aux Lettres de : CAZOU ou SHIFT sur le
serveur pas comme les autres POP au 55.74.42.71     (8 voies,24/24h)




                        A bientôt ...
```
