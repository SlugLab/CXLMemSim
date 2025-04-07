#!/bin/bash
wget https://files.rcsb.org/download/5pep.pdb
./bin/gmx pdb2gmx -f 5pep.pdb -o 5pep.gro -water spc
./bin/gmx editconf -f 5pep.gro -o 5pep-box.gro -c -d 1.0 -bt cubic
./bin/gmx solvate -cp 5pep-box.gro -cs spc216.gro -o 5pep-solv.gro -p topol.top
./bin/gmx grompp -f md.mdp -c 5pep-solv.gro -p topol.top -o input.tpr
./bin/gmx convert-tpr -s input.tpr -o topol.tpr -nsteps 1
./bin/gmx mdrun -v -s topol.tpr -ntomp 1 -ntmpi 1