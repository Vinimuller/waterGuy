import datetime
import time
import argparse
import threading

# Event Schedule Array
event_schedule = []

# Function to insert an event
def insert_event(day_of_week, hour_init, minute_init, action):
    event = {
        "dateTimeInit": datetime.time(hour=hour_init, minute=minute_init),
        "day_of_week": day_of_week,  # 0=Monday, 6=Sunday
        "Action": action,
        "executed": False  # Flag to track if the event has been executed today
    }
    event_schedule.append(event)
    print("Event added successfully.")

# Function to delete events for a specific day of the week
def delete_events(day_of_week):
    global event_schedule
    event_schedule = [event for event in event_schedule if event["day_of_week"] != day_of_week]
    print(f"Events on day {day_of_week} deleted successfully.")

# Function to list all scheduled events
def list_events():
    if not event_schedule:
        print("No events scheduled.")
        return
    print("Scheduled Events:")
    for idx, event in enumerate(event_schedule, start=1):
        print(f"{idx}. Day: {event['day_of_week']}, Time: {event['dateTimeInit']}, Action: {event['Action']}")

# Main loop to check and execute events
def main_loop():
    while True:
        now = datetime.datetime.now()
        current_day = now.weekday()  # 0=Monday, 6=Sunday
        current_time = now.time()

        for event in event_schedule:
            # Check if the event should execute based on the day and time, and if it hasn't been executed today
            if (event["day_of_week"] == current_day and
                event["dateTimeInit"].hour == current_time.hour and
                event["dateTimeInit"].minute == current_time.minute and
                not event["executed"]):
                sample_action(event["Action"])
                event["executed"] = True  # Mark as executed for today

        # Reset the 'executed' flag at midnight
        if current_time.hour == 0 and current_time.minute == 0:
            for event in event_schedule:
                event["executed"] = False

        time.sleep(60)  # Check every minute

# Sample Action function
def sample_action(action ):
    print(f"Executing sample action... {action}")

# CLI functions
def add_event():
    day_of_week = int(input("Enter day of the week (0=Monday, 6=Sunday): "))
    hour_init = int(input("Enter hour (0-23): "))
    minute_init = int(input("Enter minute (0-59): "))
    action = int(input("Enter action (0-1): "))
    
    insert_event(day_of_week, hour_init, minute_init, action)

def remove_events():
    day_of_week = int(input("Enter day of the week to delete events (0=Monday, 6=Sunday): "))
    delete_events(day_of_week)

def cli():
    while True:
        print("\nAvailable commands: add, list, remove, exit")
        command = input("Enter command: ").strip().lower()

        if command == "add":
            add_event()
        elif command == "list":
            list_events()
        elif command == "remove":
            remove_events()
        elif command == "exit":
            print("Exiting CLI...")
            break
        else:
            print("Invalid command. Please try again.")

# Run main loop in a separate thread
event_thread = threading.Thread(target=main_loop, daemon=True)
event_thread.start()

# Start CLI in main thread
if __name__ == "__main__":
    cli()
