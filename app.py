import streamlit as st
import time
from pubsub_client import PersistentClient

# ==========================
# PAGE CONFIG & STYLES
# ==========================
st.set_page_config(
    page_title="Pub-Sub Messaging System",
    layout="wide",
    page_icon="💬"
)

st.markdown("""
<style>
    body {
        background-color: #0E1117;
        color: #FAFAFA;
    }
    .block-container {
        padding-top: 2rem;
    }
    .stButton>button {
        background: linear-gradient(90deg, #00C853 0%, #009624 100%);
        color: white;
        border: none;
        border-radius: 10px;
        font-weight: 600;
        padding: 0.7rem 1.4rem;
        box-shadow: 0px 0px 10px rgba(0, 200, 83, 0.3);
        transition: 0.3s;
    }
    .stButton>button:hover {
        background: linear-gradient(90deg, #00E676 0%, #00C853 100%);
        transform: scale(1.05);
    }
    .stTextInput>div>div>input, textarea {
        background-color: #1E1E1E;
        color: #E0E0E0;
        border-radius: 8px;
    }
    .sidebar .sidebar-content {
        background-color: #111418;
    }
    h1, h2, h3 {
        color: #00C853;
    }
    .success-box {
        background-color: #1B5E20;
        padding: 10px;
        border-radius: 8px;
        color: #C8E6C9;
        margin-top: 10px;
    }
    .feed-box {
        background-color: #1E1E1E;
        border-left: 4px solid #00C853;
        padding: 12px;
        border-radius: 6px;
        margin-bottom: 10px;
        color: #FAFAFA;
        font-family: monospace;
    }
    .status-dot {
        height: 10px;
        width: 10px;
        border-radius: 50%;
        display: inline-block;
        margin-right: 8px;
    }
    .connected {
        background-color: #00C853;
    }
    .disconnected {
        background-color: #FF1744;
    }
</style>
""", unsafe_allow_html=True)

# ==========================
# SESSION STATE INIT
# ==========================
if "client" not in st.session_state:
    st.session_state.client = None
if "connected" not in st.session_state:
    st.session_state.connected = False
if "subscribed_topics" not in st.session_state:
    st.session_state.subscribed_topics = []

# ==========================
# SIDEBAR
# ==========================
st.sidebar.title("Navigation 🧭")
page = st.sidebar.radio("Go to:", ["📥 Subscriptions", "📤 Publish", "📰 Feed"])

st.sidebar.markdown("---")
st.sidebar.subheader("Server Control ⚙️")

status_color = "connected" if st.session_state.connected else "disconnected"
st.sidebar.markdown(
    f"<span class='status-dot {status_color}'></span>"
    f"{'Connected' if st.session_state.connected else 'Disconnected'}",
    unsafe_allow_html=True
)

if st.sidebar.button("Start/Connect to C++ Server"):
    try:
        name = st.sidebar.text_input("Enter your name to connect:", "User")
        client = PersistentClient(name)
        client.connect()
        st.session_state.client = client
        st.session_state.connected = True
        st.sidebar.success("✅ Connected successfully!")
    except Exception as e:
        st.sidebar.error(f"Connection failed: {e}")

st.sidebar.markdown("""---  
**Built in:**  
🧠 C++ (Lock-Free Core)  
🐍 Python + Streamlit (Frontend)
""")

# ==========================
# PAGE 1 – SUBSCRIPTIONS
# ==========================
if page == "📥 Subscriptions":
    st.title("Subscribe to Topics")
    st.write("Stay updated on topics of your choice — subscribe to start receiving messages.")

    name = st.text_input("Enter your name:")
    topic = st.text_input("Topic to subscribe:")

    if st.button("Subscribe"):
        if not st.session_state.client:
            st.error("⚠️ Connect to the server first!")
        elif topic:
            st.session_state.client.subscribe(topic)
            st.session_state.subscribed_topics.append(topic)
            st.success(f"Subscribed to topic '{topic}'")
        else:
            st.warning("Please enter a topic!")

    if st.session_state.subscribed_topics:
        st.subheader("📚 Your Subscriptions:")
        for t in st.session_state.subscribed_topics:
            st.markdown(f"- **{t}**")

# ==========================
# PAGE 2 – PUBLISH
# ==========================
elif page == "📤 Publish":
    st.title("Publish a Message")
    st.write("Send a message to all subscribers of a topic.")

    topic = st.text_input("Topic:")
    message = st.text_area("Message:")

    if st.button("Publish"):
        if not st.session_state.client:
            st.error("⚠️ Connect to the server first!")
        elif topic and message:
            st.session_state.client.publish(topic, message)
            st.success(f"Message sent to topic '{topic}' ✅")
        else:
            st.warning("Please enter both topic and message!")

# ==========================
# PAGE 3 – FEED
# ==========================
elif page == "📰 Feed":
    st.title("Live Feed")
    st.write("Messages from the topics you've subscribed to will appear below in real-time.")

    if not st.session_state.client:
        st.error("⚠️ Connect to the server first!")
    else:
        st.info("Listening for incoming messages...")

        placeholder = st.container()
        while True:
            msg = st.session_state.client.get_message()
            if msg:
                with placeholder:
                    st.markdown(f"<div class='feed-box'>{msg}</div>", unsafe_allow_html=True)
            time.sleep(0.2)
