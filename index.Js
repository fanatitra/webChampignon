const express = require('express');
const path = require('path');
const app = express();
const port = process.env.PORT || 3000;

// Servir les fichiers statiques (HTML, CSS, JS) du dossier actuel
app.use(express.static(__dirname));

// Envoyer le fichier principal quand on arrive sur le site
app.get('/', (req, res) => {
  res.sendFile(path.join(__dirname, 'Champignongroupe2.html'));
});

app.listen(port, () => {
  console.log(`Serveur lancé sur http://localhost:${port}`);
  console.log('Appuyez sur CTRL+C pour arrêter');
});