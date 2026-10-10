# Project Documentation & Commit Log

## Project Details
- **Project Name:** DSA Project 1
- **Repository:** Time-travel-debugger

## Commit Log

## commit 1
- ** We have commit the initial server.cpp after creating the stack and first 3 functions of 0x0 .
- ** We have also created a markdown file for keeping record of our progress
## commit2
- **we have changed the readbinaryfile code a bit at first i thought the binary file will contain text so thats why i read it like that 
- ** now we have changed it so now it read byte by byte 
- ** we have also added the validation function which checks if the code written is valid ... i mean for every func there is a func_end and no nested functions 

## commit 3
-**We have done the stage 1 of this project here in which what we did is that first we wrote the resolver record into the file 
-** then after that we wrote a function for reading it from the file and lastly we wrote resolverprogram function
-** in this what we did is that we will check our line first word if its a "func" meaninf we will store its offset and name in the func array
-** If its "call" then we will loop through the funcarray and then find the same name func from where we will get the byte offset
-** if we didnot find the byte offset in resolve.bin then we will simply just added it in the patches array
-** otherwise we will writeitinto the resolver.bin file
## commit 4
-** we wrote a function build snapshot in which first we create a snapshot object and then we will call our snapshot into method of stack
-** we also created a destructor for stack

## commit 5 
-** in this commit we have written the tokenize line code in which we tokenize our line and give them types 
## commit 6 
-** in this commit we have added the execution program code 
## commit 7
-** we have added timelinenode

# commit 8
-** This is hopefully our lastcommit what we did in this is that we wrote our lastfucntion writetdbg
# COMMIT 9
--* In this new commit i have made some chnages in my execute program after checking that my previous code had some problems + it doesnot give us errors now in this I have added run time errors
# COMMIT 10
--* IN THIS COMMIT WE HAVE ADDED RUN TIME ERROR IN ALL THE PLACES WHERE ERROR COULD HAPPEN IN THE CODE 
