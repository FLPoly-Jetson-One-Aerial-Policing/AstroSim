using UnityEngine;

/// <summary>
/// Simple but complete helicopter / personal-drone controller.
/// Put this on the root "Helicopter" object (the one with the Rigidbody).
///
/// CONTROLS (change them in the Inspector if you like)
///   R            Engine on / off
///   Space        Climb          Left Shift   Descend
///   W / S        Pitch (nose down = forward / nose up = backward)
///   A / D        Roll (bank left / right)
///   Q / E        Yaw (turn left / right)
///   C            Switch camera (handled by CameraSwitcher.cs)
/// </summary>
[RequireComponent(typeof(Rigidbody))]
public class HelicopterController : MonoBehaviour
{
    [Header("Keys")]
    public KeyCode engineKey    = KeyCode.R;
    public KeyCode climbKey     = KeyCode.Space;
    public KeyCode descendKey   = KeyCode.LeftShift;
    public KeyCode yawLeftKey   = KeyCode.Q;
    public KeyCode yawRightKey  = KeyCode.E;

    [Header("Engine")]
    public bool  startWithEngineOn = false;
    public float spoolUpTime   = 3f;   // seconds to reach full rotor speed
    public float spoolDownTime = 5f;   // seconds to wind down after engine off

    [Header("Lift (units = meters per second squared)")]
    public float climbPower = 6f;      // extra push from Space / Shift on top of hover
    public float maxSpeed   = 30f;     // soft speed limit (m/s)

    [Header("Rotation (higher = snappier)")]
    public float pitchPower = 3f;
    public float rollPower  = 3f;
    public float yawPower   = 2f;

    [Header("Stability")]
    public bool  autoLevel     = true;
    public float levelStrength = 2f;   // how hard it rights itself when you let go

    [Header("Ground Effect (cushion near the ground)")]
    public float groundEffectHeight = 3f;   // within this many meters of ground
    public float groundEffectBoost  = 2f;   // extra upward acceleration at ground level

    [Header("Visual Parts (optional - drag child objects here)")]
    public Transform mainRotor;             // spins around its Y axis
    public Transform tailRotor;             // spins around its X axis
    public float mainRotorMaxRPM = 1500f;   // degrees/second at full speed
    public float tailRotorMaxRPM = 2500f;

    [Header("Audio (optional)")]
    public AudioSource engineSound;         // a looping sound on an AudioSource
    public float minPitch = 0.3f;
    public float maxPitch = 1.4f;

    [Header("HUD")]
    public bool showHUD = true;

    // ---- internal state ----
    Rigidbody rb;
    bool  engineOn;
    float rotorSpeed;                  // 0 (stopped) to 1 (full)
    float collective;                  // -1..+1
    float pitchInput, rollInput, yawInput;

    Vector3 lastPosition;
    float   currentSpeed;              // meters per second
    float   altitude;                  // meters above ground (raycast)

    void Awake()
    {
        rb = GetComponent<Rigidbody>();
        rb.centerOfMass = new Vector3(0f, -0.3f, 0f); // slightly low = more stable

        engineOn   = startWithEngineOn;
        rotorSpeed = engineOn ? 1f : 0f;
        lastPosition = rb.position;

        if (engineSound != null)
        {
            engineSound.loop = true;
            if (!engineSound.isPlaying) engineSound.Play();
        }
    }

    void Update()
    {
        ReadInput();
        UpdateRotorSpeed();
        SpinRotors();
        UpdateAudio();
    }

    void FixedUpdate()
    {
        MeasureSpeedAndAltitude();
        ApplyLift();
        ApplyRotation();
        if (autoLevel) ApplyAutoLevel();
        ApplySpeedLimit();
    }

    // ------------------------------------------------------------
    // INPUT
    // ------------------------------------------------------------
    void ReadInput()
    {
        if (Input.GetKeyDown(engineKey)) engineOn = !engineOn;

        float up   = Input.GetKey(climbKey)   ? 1f : 0f;
        float down = Input.GetKey(descendKey) ? 1f : 0f;
        collective = up - down;

        pitchInput = Input.GetAxis("Vertical");      // W/S, Up/Down arrows
        rollInput  = -Input.GetAxis("Horizontal");   // A/D, Left/Right arrows

        float right = Input.GetKey(yawRightKey) ? 1f : 0f;
        float left  = Input.GetKey(yawLeftKey)  ? 1f : 0f;
        yawInput = right - left;
    }

    // ------------------------------------------------------------
    // ENGINE / ROTORS
    // ------------------------------------------------------------
    void UpdateRotorSpeed()
    {
        float target = engineOn ? 1f : 0f;
        float time   = engineOn ? spoolUpTime : spoolDownTime;
        rotorSpeed = Mathf.MoveTowards(rotorSpeed, target, Time.deltaTime / Mathf.Max(0.01f, time));
    }

    void SpinRotors()
    {
        if (mainRotor != null)
            mainRotor.Rotate(0f, mainRotorMaxRPM * rotorSpeed * Time.deltaTime, 0f, Space.Self);
        if (tailRotor != null)
            tailRotor.Rotate(tailRotorMaxRPM * rotorSpeed * Time.deltaTime, 0f, 0f, Space.Self);
    }

    void UpdateAudio()
    {
        if (engineSound == null) return;
        engineSound.pitch  = Mathf.Lerp(minPitch, maxPitch, rotorSpeed);
        engineSound.volume = Mathf.Clamp01(rotorSpeed * 1.2f);
    }

    // ------------------------------------------------------------
    // PHYSICS
    // ------------------------------------------------------------
    void MeasureSpeedAndAltitude()
    {
        currentSpeed = ((rb.position - lastPosition) / Time.fixedDeltaTime).magnitude;
        lastPosition = rb.position;

        // Ray starts a little above our position so we don't hit our own collider
        Vector3 origin = transform.position + Vector3.up * 0.1f;
        if (Physics.Raycast(origin, Vector3.down, out RaycastHit hit, 1000f, ~0, QueryTriggerInteraction.Ignore)
            && hit.collider.attachedRigidbody != rb)
            altitude = hit.distance;
        else
            altitude = transform.position.y;
    }

    void ApplyLift()
    {
        // Hover acceleration cancels gravity exactly when rotors are at full speed.
        // Lift scales with rotorSpeed, so with the engine off you simply fall.
        float hover  = Physics.gravity.magnitude;
        float thrust = (hover + collective * climbPower) * rotorSpeed;

        // Ground effect: extra cushion when close to the floor
        if (altitude < groundEffectHeight)
        {
            float t = 1f - (altitude / groundEffectHeight);
            thrust += groundEffectBoost * t * rotorSpeed;
        }

        // Thrust follows the helicopter's own "up", so tilting = moving
        rb.AddForce(transform.up * thrust, ForceMode.Acceleration);
    }

    void ApplyRotation()
    {
        // Control authority fades as the rotors spin down
        float authority = rotorSpeed;

        rb.AddRelativeTorque(Vector3.right   * pitchInput * pitchPower * authority, ForceMode.Acceleration);
        rb.AddRelativeTorque(Vector3.forward * rollInput  * rollPower  * authority, ForceMode.Acceleration);
        rb.AddRelativeTorque(Vector3.up      * yawInput   * yawPower   * authority, ForceMode.Acceleration);
    }

    void ApplyAutoLevel()
    {
        bool noTiltInput = Mathf.Approximately(pitchInput, 0f) && Mathf.Approximately(rollInput, 0f);
        if (!noTiltInput) return;

        // Cross product = axis that swings our "up" back toward world "up"
        Vector3 correction = Vector3.Cross(transform.up, Vector3.up);
        rb.AddTorque(correction * levelStrength * rotorSpeed, ForceMode.Acceleration);
    }

    void ApplySpeedLimit()
    {
        if (currentSpeed <= maxSpeed) return;

        // Push against the direction of travel in proportion to how far over the limit we are
        Vector3 travelDir = (rb.position - lastPosition).sqrMagnitude > 0.0001f
            ? (rb.position - lastPosition).normalized
            : Vector3.zero;
        float over = currentSpeed - maxSpeed;
        rb.AddForce(-travelDir * over, ForceMode.Acceleration);
    }

    // ------------------------------------------------------------
    // HUD (simple on-screen text, no UI setup needed)
    // ------------------------------------------------------------
    void OnGUI()
    {
        if (!showHUD) return;

        GUIStyle style = new GUIStyle(GUI.skin.label) { fontSize = 18 };
        style.normal.textColor = Color.white;

        GUI.Label(new Rect(15, 10, 400, 30), "Engine: " + (engineOn ? "ON" : "OFF") + "   (R)", style);
        GUI.Label(new Rect(15, 35, 400, 30), "Rotor:  " + Mathf.RoundToInt(rotorSpeed * 100f) + "%", style);
        GUI.Label(new Rect(15, 60, 400, 30), "Alt:    " + altitude.ToString("F1") + " m", style);
        GUI.Label(new Rect(15, 85, 400, 30), "Speed:  " + currentSpeed.ToString("F1") + " m/s", style);
    }
}