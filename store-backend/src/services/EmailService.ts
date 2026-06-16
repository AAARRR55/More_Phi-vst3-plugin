import { env } from "../config/env.js";
import { logger } from "../lib/logger.js";

export interface LicenseEmailInput {
  to: string;
  licenseKey: string;
  productName: string;
  downloadUrl: string;
  setPasswordLink?: string;
}

export const EmailService = {
  async sendLicenseEmail(input: LicenseEmailInput) {
    const subject = `Your ${input.productName} license key`;
    const html = this.buildLicenseEmailHtml(input);
    const text = this.buildLicenseEmailText(input);

    switch (env.EMAIL_PROVIDER) {
      case "resend":
        await this.sendResend(input.to, subject, html, text);
        break;
      case "smtp":
        await this.sendSmtp(input.to, subject, html, text);
        break;
      case "console":
      default:
        logger.info(
          {
            to: input.to,
            subject,
            licenseKey: input.licenseKey,
          },
          "[EMAIL] License email"
        );
    }
  },

  buildLicenseEmailHtml(input: LicenseEmailInput): string {
    return `
      <html>
        <body style="font-family: sans-serif; line-height: 1.5;">
          <h1>Thank you for purchasing ${input.productName}!</h1>
          <p>Your license key is:</p>
          <p style="font-size: 1.5rem; font-weight: bold; background: #f4f4f4; padding: 12px; border-radius: 6px; display: inline-block;">
            ${input.licenseKey}
          </p>
          <p>Download your plugin: <a href="${input.downloadUrl}">${input.downloadUrl}</a></p>
          ${input.setPasswordLink ? `<p>Create your account password: <a href="${input.setPasswordLink}">${input.setPasswordLink}</a></p>` : ""}
          <p>Need help? Contact support at <a href="mailto:support@morephi.example">support@morephi.example</a>.</p>
        </body>
      </html>
    `;
  },

  buildLicenseEmailText(input: LicenseEmailInput): string {
    return [
      `Thank you for purchasing ${input.productName}!`,
      "",
      `Your license key is: ${input.licenseKey}`,
      "",
      `Download: ${input.downloadUrl}`,
      input.setPasswordLink ? `Set your password: ${input.setPasswordLink}` : "",
      "",
      "Need help? Contact support@morephi.example",
    ]
      .filter(Boolean)
      .join("\n");
  },

  async sendResend(to: string, subject: string, html: string, text: string) {
    if (!env.RESEND_API_KEY) {
      throw new Error("RESEND_API_KEY is not configured");
    }

    const response = await fetch("https://api.resend.com/emails", {
      method: "POST",
      headers: {
        Authorization: `Bearer ${env.RESEND_API_KEY}`,
        "Content-Type": "application/json",
      },
      body: JSON.stringify({
        from: env.EMAIL_FROM,
        to,
        subject,
        html,
        text,
      }),
    });

    if (!response.ok) {
      const body = await response.text();
      throw new Error(`Resend API error: ${response.status} ${body}`);
    }
  },

  async sendSmtp(to: string, subject: string, _html: string, _text: string) {
    // SMTP support requires a transport library. Keeping this dependency-free
    // by default; if EMAIL_PROVIDER=smtp is used, install nodemailer and wire
    // it here. For now we log and do not silently drop.
    logger.warn(
      { to, subject },
      "SMTP provider selected but not implemented. Install nodemailer to enable SMTP delivery."
    );
    throw new Error("SMTP email provider is not fully implemented in this template");
  },
};
