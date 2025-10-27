import random
# Define the name of the file to be created.
file_name = "my_file.txt"

# Define the content to be written to the file.
# Using a list of strings is a common way to manage multiple lines.
sensors = ["A", "B", "C", "D"]
with open("outfile.data", "w") as file_handler:
    for i in range(10000000):
        line = f"temperature[sensor=\"{random.choice(sensors)}\", site=\"B\", bomb='BBB'], degrees={random.randint(1, 99):02}\n"
        file_handler.write(line)
