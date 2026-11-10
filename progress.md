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
# Commit 11
--* In this commit we have made quite alot of changes like i have changed some parts of my error where i was getting error today after testing and we have also created a new main which catches the error and i have created a function which prints output onto consolve for my understanding 
# Commit 12
--* In this commit we are moving to client side and now have created 3 funcions of client .
#commit 13
--* IN this commit we have added open close tdbg file  and reading every stack frames  
# Commit 14
--* So here we added initsession,close session , showcurrent which displays the current snapshot the user is looking at along with move by (moving foward and backward in time)

# Commit 15
--* We have added functions in which user can jump to next state and commandstart fucntion which creates a new debugging session while protecting your current active one 
#commit 16 
--* Now here  we have added some functions for our console like printing the entire timeline uptil now and what commands we have
# commit 17
--* hopefully our last commit for client.cpp in this commit we have created a main  for it 
# commit 18
--* HERE WE HAVE STARTED TO WORK ON LOADBALANCER FILE IN THIS PART TILL NO WE HAVE MADE OUR CUSTOM QUEUE , SENDALL FUNCTION AND SEND ERROR FUNCTION WHICH GIVES ERROR MSG  TO USER  INCASE SOMETHING  GOES WRONG
# commit 19
--* here i have added and modified my code so it can run on windows as well more over here we have implemented waking up the server if its down 

# Commit 20
--* here we have added the function relayoneclient
# commit 21 
--* here we have added fucntions in server.cpp so it can respond to the request of clients added some new functions onto this 
