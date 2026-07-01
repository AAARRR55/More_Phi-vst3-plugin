import { initializeApp, getApps, getApp } from 'firebase/app'
import { getAnalytics, isSupported } from 'firebase/analytics'
import { getAuth } from 'firebase/auth'

const firebaseConfig = {
  apiKey: "AIzaSyCfQWEjsohugDXOUOoF9QtTr9WiKwdQqkA",
  authDomain: "more-phi.firebaseapp.com",
  projectId: "more-phi",
  storageBucket: "more-phi.firebasestorage.app",
  messagingSenderId: "573488071869",
  appId: "1:573488071869:web:2e542e5f0f4c81f093a3d4",
  measurementId: "G-MB066RF4DG"
}

// Initialize Firebase (SSR Safe, prevents double-initialization on hot-reload)
const app = getApps().length === 0 ? initializeApp(firebaseConfig) : getApp()

// Initialize Auth
const auth = getAuth(app)

// Initialize Analytics (SSR Safe, only runs in browser environment)
let analytics = null
if (typeof window !== 'undefined') {
  isSupported().then((supported) => {
    if (supported) {
      analytics = getAnalytics(app)
    }
  })
}

export { app, auth, analytics }
