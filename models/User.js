const level = require('level');
const db = level('./userDB');  // Path to your LevelDB instance

// Function to add a user to the LevelDB database
function addUser(userId, userData) {
  db.put(userId, JSON.stringify(userData), (err) => {
    if (err) {
      console.error('Error storing user in LevelDB:', err);
      return;
    }
    console.log('User added successfully');
  });
}

// Function to get a user by ID from the database
function getUser(userId, callback) {
  db.get(userId, (err, value) => {
    if (err) {
      console.error('Error fetching user from LevelDB:', err);
      return;
    }
    callback(JSON.parse(value));  // Parse and return the stored user data
  });
}

// Function to delete a user by ID from the database
function deleteUser(userId) {
  db.del(userId, (err) => {
    if (err) {
      console.error('Error deleting user from LevelDB:', err);
      return;
    }
    console.log('User deleted successfully');
  });
}

module.exports = { addUser, getUser, deleteUser };
