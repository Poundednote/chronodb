import random
import sys
# Define the name of the file to be created.
file_name = "my_file.txt"

# Define the content to be written to the file.
# Using a list of strings is a common way to manage multiple lines.
sensors = ["A", "B", "C", "D"]
filetype = "new"

if len(sys.argv) > 1:
    filetype = str(sys.argv[1])

n_rows = 1000000
if filetype == "new":
    with open("outfile.data", "w") as file_handler:
        for i in range(n_rows):
            line = f"temperature {i} [sensor=\"{random.choice(sensors)}\", site=\"B\", bomb='BBB'] degrees={random.randint(1, 99):02}, columnA={random.randint(1,99)/100}'\n"
            file_handler.write(line)

elif filetype == "exist":
    with open("outfile.data", "w") as file_handler:
        for i in range(n_rows):
            if random.randint(0, 1):
                line = f"table0 {i} [] testcolumn0={random.randint(0, 999)}, testcolumn1={random.randint(0, 999)}\n"
            else:
                line = f"table0 {i} [] testcolumn0={random.randint(0, 999)}, newcol={random.randint(0, 999)}\n"
            file_handler.write(line)


elif filetype == "o3":
    with open("outfile.data", "w") as file_handler:
        for i in range(n_rows):
            line = f"temperature {n_rows - i} [sensor=\"{random.choice(sensors)}\", site=\"B\", bomb='BBB'] degrees={random.randint(1, 99):02}, columnA={random.randint(1,99)/100}'\n"
            file_handler.write(line)
