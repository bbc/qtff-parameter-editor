#!/bin/bash
## 
## Copyright (C) 2017, British Broadcasting Corporation
## All Rights Reserved.
## 
## Author: Manish Pindoria (manish.pindoria@bbc.co.uk)
##         Philip de Nier  (philipn@rd.bbc.co.uk)  
## 
## Redistribution and use in source and binary forms, with or without
## modification, are permitted provided that the following conditions are met:
## 
##     * Redistributions of source code must retain the above copyright notice,
##       this list of conditions and the following disclaimer.
##     * Redistributions in binary form must reproduce the above copyright
##       notice, this list of conditions and the following disclaimer in the
##       documentation and/or other materials provided with the distribution.
##     * Neither the name of the British Broadcasting Corporation nor the names
##       of its contributors may be used to endorse or promote products derived
##       from this software without specific prior written permission.
## 
## THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
## AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
## IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
## ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
## LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
## CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
## SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
## INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
## CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
## ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
## POSSIBILITY OF SUCH DAMAGE.
## 
 
ffprobe_exe=""
 
#define the tabels as globals
primaries=("Reserved"
           "ITU-R BT.709"
           "Unspecified" 
           "Reserved"
           "ITU-R BT.470M" 
           "ITU-R BT.470BG" 
           "SMPTE 170M"
           "SMPTE 240M"
           "FILM"
           "ITU-R BT.2020" 
           "SMPTE ST 428-1" 
           "DCI P3"
           "P3 D65") 



###########################################################################################

tf=("Reserved"
    "ITU-R BT.709" 
    "Unspecified"
    "Reserved"
    "Gamma 2.2 curve"
    "Gamma 2.8 curve"
    "SMPTE 170M"
    "SMPTE 240M" 
    "Linear"
    "Log"
    "Log Sqrt"
    "IEC 61966-2-4" 
    "ITU-R BT.1361 Extended Colour Gamut"
    "IEC 61966-2-1" 
    "ITU-R BT.2020 10 bit" 
    "ITU-R BT.2020 12 bit" 
    "SMPTE ST 2084 (PQ)"
    "SMPTE ST 428-1"
    "ARIB STD-B67 (HLG)")


###########################################################################################


matrix=("GBR"
        "BT709"
        "Unspecified" 
        "Reserved"
        "FCC"
        "BT470BG"
        "SMPTE 170M"
        "SMPTE 240M"
        "YCOCG"
        "BT2020 Non-constant Luminance"
        "BT2020 Constant Luminance)")
  

###########################################################################################
  
  
printPrimariesTable()
{
  echo "The colour primaries of the video. For clarity, the value and meanings" 
  echo "for Primaries are adopted from Table 2 of ISO/IEC 23001-8:2013/DCOR1." 
  
  for i in $(seq 0 10)
  do
    echo ${i}: ${primaries[${i}]}
  done 
  
  echo
}


###########################################################################################


printTFTable()
{
  echo "TransferCharacteristics, 5bits, The transfer characteristics of the video." 
  echo "For clarity, the value and meanings for TransferCharacteristics"
  echo "1-15 are adopted from Table 3 of ISO/IEC 23001-8:2013/DCOR1." 
  echo "TransferCharacteristics 16-18 are proposed values."
  
  for i in $(seq 0 18)
  do
    echo ${i}: ${tf[${i}]}
  done
  
  echo 
}


###########################################################################################


printMatrixTable()
{
  echo "The Matrix Coefficients of the video used to derive luma and chroma values"
  echo "from reg, green, and blue color primaries."
  echo "For clarity, the value and meanings for MatrixCoefficients are adopted from"
  echo "Table 4 of ISO/IEC 23001-8:2013/DCOR1."

  for i in $(seq 0 10)
  do
    echo ${i}: ${matrix[${i}]}
  done
}


###########################################################################################


outputHelp()
{ 
   echo 
   echo "qtff-parameter-editor.sh"
   echo 
   echo "This script can be used to edit the primaries, transfer functions and matrix"
   echo "fields in a .mov file, it can be extended for further editting if required." 
   echo 
   echo " Usage:"
   echo "         qtff-parameter-editor.sh [--help] [-h] [-p priValue] "
   echo "                                        [-t tfValue] "
   echo "                                        [-m matValue] "
   echo "                                        InputFile"
   echo "                                        OutputFile"
   echo " or "
   echo "         qtff-parameter-editor.sh InputFile"
   echo 
   echo " Where:"
   echo "   -h, --help :               Displays this help page"
   echo "   -p --primaries priValue:   Where priValue is the required primaries value number"
   echo "   -t --tf tfValue:           Where tfValue is the required transfer function value number" 
   echo "   -m --matrix matValue:      Where matValue is the required matrix function value number" 
   echo "   InputFile:                 Source mov file" 
   echo "   OutputFile:                Output mov file (Can be the same as the Input file)" 
   echo 
   echo "If only an Input file is provided (as in the second example, the script will return "
   echo "the information for the input file" 
   echo 
   echo "Example 1: ./qtff-parameter-editor.sh input.mov"
   echo "           Return video parameter information for input.mov"
   echo
   echo "Example 2: ./qtff-parameter-editor.sh -p 1 -t 1 -m 1 input.mov output.mov "
   echo "           Will create a copy with input.mov and modify the video parameters to bt709 "
   echo "           primaries, transfer function and matrix."
   echo
   echo "Example 3: ./qtff-parameter-editor.sh --primaries 9 --tf 18 --matrix 9 input.mov input.mov "
   echo "           Will modify the video parameters in situ to bt2020 primaries, HLG transfer "
   echo "           function and bt2020 non constant luminance matrix" 
   
   echo -e "\n\nThe values for the options are given by:\n\n"
   printPrimariesTable
   printTFTable
   printMatrixTable
   exit
}


###########################################################################################



outputInfoMovWrapper()
{
  ipFile=$1
  ${dir}/src/movdump $ipFile > ${ipFile%.mov}.dump
  colrline=$(grep -hnr "colr:"  ${ipFile%.mov}.dump)
  colrline=${colrline%%:*}
  
  if [ "$colrline" == "" ]
  then
    echo "colr atom not located, unable to display any further information or make any edits"
    exit    
  else
    head -n $(($colrline+4)) ${ipFile%.mov}.dump | tail -n 5 | sed -e 's/^[[:space:]]*//' > ${ipFile%.mov}_useful.dump
    thisPrimary=$(grep "primaries" ${ipFile%.mov}_useful.dump)
    thistf=$(grep "transfer" ${ipFile%.mov}_useful.dump)
    thismatrix=$(grep "matrix" ${ipFile%.mov}_useful.dump)
    
    thisPrimary=${thisPrimary#*:}
    thistf=${thistf#*:}
    thismatrix=${thismatrix#*:}
    thisPrimary=$(echo ${thisPrimary} | tr -d '[:space:]')
    thistf=$(echo ${thistf} | tr -d '[:space:]')
    thismatrix=$(echo ${thismatrix} | tr -d '[:space:]')
    
    echo 
    echo "File = $ipFile"
    echo 
    echo "MOV Wrapper Information (COLR atom)"
    echo "Primary = $thisPrimary (${primaries[${thisPrimary}]})"
    echo "Transfer Function = $thistf (${tf[${thistf}]})"
    echo "Matrix = $thismatrix (${matrix[${thismatrix}]})"
    echo 
    
    
  fi
}


###########################################################################################


outputInfoProRes()
{
  ipFile=$1
  leaf=${ipFile%.mov}
  ${ffprobe_exe} -loglevel panic -show_packets -select_streams v:0 ${ipFile} | grep pos > ${leaf}_offsets.txt
  ${dir}/src/rdd36mod -s -o ${leaf}_offsets.txt ${ipFile} > ${leaf}_rdd36mod.txt 2>&1
  
  #cat ${leaf}_rdd36mod.txt
  errorline=$(grep "failed" ${leaf}_rdd36mod.txt)
  if [ "$errorline" == "" ]
  then
  
    thisPrimary=$(grep "primaries" ${leaf}_rdd36mod.txt)
    thistf=$(grep "transfer" ${leaf}_rdd36mod.txt)
    thismatrix=$(grep "matrix" ${leaf}_rdd36mod.txt)
    
    thisPrimary=${thisPrimary#*:}
    thistf=${thistf#*:}
    thismatrix=${thismatrix#*:}
    thisPrimary=$(echo ${thisPrimary} | tr -d '[:space:]')
    thistf=$(echo ${thistf} | tr -d '[:space:]')
    thismatrix=$(echo ${thismatrix} | tr -d '[:space:]')
    
    echo 
    echo "SMPTE RDD 36 (Apple ProRes) Information"
    echo "Primary = $thisPrimary (${primaries[${thisPrimary}]})"
    echo "Transfer Function = $thistf (${tf[${thistf}]})"
    echo "Matrix = $thismatrix (${matrix[${thismatrix}]})"
    echo  
    
  else
    echo "ProRes header not found, unable to edit this file"
    exit
  fi
}


###########################################################################################



cleanup()
{
  ipFile=$1
  leaf=${ipFile%.mov}
  rm -f ${leaf}_useful.dump
  rm -f ${leaf}.dump
  rm -f ${leaf}_rdd36mod.txt
  rm -f ${leaf}_offsets.txt
}


###########################################################################################



cloneMovAndModify()
{
  ipFile=$1 
  opFile=$2 
  newPrim=$3 
  newTF=$4 
  newMatrix=$5
  
  leaf=${ipFile%.mov}
  
  outputInfoMovWrapper $ipFile
  outputInfoProRes $ipFile
  
  if [ "$ipFile" = "$opFile" ]
  then
    echo "Output file is the same as Input, are you sure you want to continue,"
    echo "type yes or no, followed by [ENTER] "
    read response
    if [ "$response" = "yes" ]
    then
      echo "Adjusting file in situ ..."
    else
      echo "Not updating file ..."
      echo "Exiting"
      cleanup $ipFile
      exit
    fi    
    
    echo "Processing with adjustment ..."
  else
    echo "Cloning ..."
    pvavail=$(command -v pv)
    if [[ -n "${pvavail/[ ]*\n/}" ]] 
    then
      pv $ipFile > $opFile
    else 
      cp -v $ipFile $opFile
    fi
  fi
  
  #get the offset
  offset=$(grep -hr "colr" ${ipFile%.mov}_useful.dump)
  offset=${offset#*o=}
  offset=${offset%(*}
  offset=$(echo ${offset} | tr -d '[:space:]')
  
  if [ "$newPrim" != "-1" ]
  then
    echo "Modifying the primary ..."
    primhex=$(printf "%02x" $newPrim)    
    printf "\x${primhex}" | dd conv=notrunc of=$opFile bs=1 seek=$(($offset + 13)) &> /dev/null 
    ${dir}/src/rdd36mod -o ${leaf}_offsets.txt -p $newPrim  ${opFile}    
  fi  
  
  if [ "$newTF" != "-1" ]
  then
    echo "Modifying the transfer function ..." 
    tfhex=$(printf "%02x" $newTF)   
    printf "\x${tfhex}" | dd conv=notrunc of=$opFile bs=1 seek=$(($offset + 15)) &> /dev/null 
    ${dir}/src/rdd36mod -o ${leaf}_offsets.txt -t $newTF  ${opFile}   
  fi 
  
  if [ "$newMatrix" != "-1" ]
  then
    echo "Modifying the matrix ..." 
    matrixhex=$(printf "%02x" $newMatrix) 
    printf "\x${newMatrix}" | dd conv=notrunc of=$opFile bs=1 seek=$(($offset + 17)) &> /dev/null 
   ${dir}/src/rdd36mod -o ${leaf}_offsets.txt -m $newMatrix  ${opFile}       
  fi 
  
  outputInfoMovWrapper $opFile
  outputInfoProRes $opFile
}


###########################################################################################


checkffprobe()
{
  if type ffprobe 2>/dev/null
  then
    echo "Found ffprobe in the PATH"
    ffprobe_exe=$(which ffprobe)
  else
    echo "ffprobe not found in the PATH,"
    echo "Please provide the location of ffprobe, followed by [ENTER] "
    read ffprobe_exe
    if type ${ffprobe_exe} >/dev/null 2>&1
    then
      echo "Found ffprobe in the location ${ffprobe_exe}"
    else
      echo "ffprobe not found, it is required"
      echo "Exiting"
      exit
    fi    
  fi
}


###########################################################################################

dir=$(dirname $0)

if [ "$#" = "0" ]
then 
   outputHelp
elif [ "$#" = "1" ] 
then
  if [ $1 == "-h" ] || [ $1 == "--help" ] 
  then
    outputHelp
  else
    checkffprobe
    outputInfoMovWrapper $1
    outputInfoProRes $1
    cleanup $1
  fi
else
  newPrim=-1
  newTF=-1
  newMatrix=-1
  while  [ "$#" != "2" ]
  do   
    if [ "$1" == "--primaries" ]  || [ "$1" == "-p" ]
    then  
      newPrim=$2 
      shift 2
    elif [ "$1" == "--tf" ] || [ "$1" == "-t" ]
    then 
      newTF=$2
      shift 2
    elif  [ "$1" == "--matrix" ] || [ "$1" == "-m" ]
    then    
      newMatrix=$2
      shift 2  
    else
      echo "Unknown option $1 - exiting"
      exit      
    fi    
  done
  
  checkffprobe
  cloneMovAndModify $1 $2 $newPrim $newTF $newMatrix
  cleanup $1
  cleanup $2
fi
   

###########################################################################################



   


