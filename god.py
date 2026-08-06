import random

def create_god():
    god_names = ['Zeus', 'Hera', 'Poseidon', 'Athena', 'Ares', 'Aphrodite', 'Hephaestus', 'Hermes', 'Artemis', 'Apollo', 'Demeter', 'Dionysus']
    powers = ['lightning', 'wisdom', 'the sea', 'war', 'love', 'craftsmanship', 'messengers', 'hunting', 'the sun', 'agriculture', 'wine']
    
    god_name = random.choice(god_names)
    god_power = random.choice(powers)
    
    return f"The mighty {god_name}, god of {god_power}!"

print(create_god())