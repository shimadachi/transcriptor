import {createRoot} from 'react-dom/client';

import './styles/fonts.css';
import './styles/style.css';

import {App} from './components/App.jsx';
import {paintSavedPrefs} from './state/prefs.js';

// Theme and language first, from what this window remembered: the first frame
// is then already in them, rather than a flash of the defaults.
paintSavedPrefs();

createRoot(document.getElementById('root')).render(<App />);
